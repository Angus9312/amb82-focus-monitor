/*
 * Ameba AMB82-MINI：YOLO 人員／手機辨識與專注事件判斷
 *
 * 判斷原則：
 * 1. person 與 cell phone 都達到 60% 信心門檻。
 * 2. person 框覆蓋至少 20% 的 phone 框。
 * 3. 上述條件連續 3 秒才提醒並計數。
 * 4. 計數後，必須在學生仍清楚可見時連續 3 秒沒有手機，才重新允許計數。
 * 5. 低信心、遮擋或不完整結果只會中斷計時，不會當作分心事件。
 */

#include "WiFi.h"
#include "StreamIO.h"
#include "VideoStream.h"
#include "RTSP.h"
#include "NNObjectDetection.h"
#include "VideoStreamOverlay.h"

#define STREAM_CHANNEL 0
#define NN_CHANNEL 3
#define NN_WIDTH 576
#define NN_HEIGHT 320

const int PERSON_CLASS = 0;
const int PHONE_CLASS = 67;
const int CONFIDENCE_THRESHOLD = 60;
const int AMBIGUOUS_SCORE = 25;
const float PHONE_OVERLAP_THRESHOLD = 0.20f;
const unsigned long TRIGGER_MS = 3000;
const unsigned long REARM_MS = 3000;
const unsigned long RESULT_STALE_MS = 900;

char ssid[] = "YOUR_WIFI_SSID";
char pass[] = "YOUR_WIFI_PASSWORD";

VideoSetting streamConfig(VIDEO_FHD, 30, VIDEO_H264, 0);
VideoSetting nnConfig(NN_WIDTH, NN_HEIGHT, 10, VIDEO_RGB, 0);
NNObjectDetection detector;
RTSP rtsp;
StreamIO streamLink(1, 1);
StreamIO nnLink(1, 1);
WiFiServer statusServer(8080, TCP_MODE, NON_BLOCKING_MODE);

struct Box {
    float x1, y1, x2, y2;
    int score;
    bool valid;
};

Box personBox = {0, 0, 0, 0, 0, false};
Box phoneBox = {0, 0, 0, 0, 0, false};
bool reliable = false;
bool overlapNow = false;
bool distracted = false;
bool armed = true;
unsigned long personStart = 0;
unsigned long phoneStart = 0;
unsigned long overlapStart = 0;
unsigned long clearStart = 0;
unsigned long lastResultAt = 0;
unsigned long eventCount = 0;

unsigned long elapsedSince(unsigned long started, unsigned long now)
{
    return started == 0 ? 0 : now - started;
}

float overlapOfPhone(const Box &person, const Box &phone)
{
    float left = max(person.x1, phone.x1);
    float top = max(person.y1, phone.y1);
    float right = min(person.x2, phone.x2);
    float bottom = min(person.y2, phone.y2);
    float intersection = max(0.0f, right - left) * max(0.0f, bottom - top);
    float phoneArea = max(0.0f, phone.x2 - phone.x1) * max(0.0f, phone.y2 - phone.y1);
    return phoneArea > 0.0001f ? intersection / phoneArea : 0.0f;
}

void updateTimer(bool condition, unsigned long &started, unsigned long now)
{
    if (condition) {
        if (started == 0) started = now;
    } else {
        started = 0;
    }
}

void updateDecision(unsigned long now, bool ambiguous)
{
    bool personDetected = personBox.valid;
    bool phoneDetected = phoneBox.valid;
    reliable = personDetected && !ambiguous;
    float ratio = (personDetected && phoneDetected) ? overlapOfPhone(personBox, phoneBox) : 0.0f;
    overlapNow = reliable && phoneDetected && ratio >= PHONE_OVERLAP_THRESHOLD;

    updateTimer(personDetected, personStart, now);
    updateTimer(phoneDetected, phoneStart, now);
    updateTimer(overlapNow, overlapStart, now);

    distracted = overlapNow && elapsedSince(overlapStart, now) >= TRIGGER_MS;
    if (armed && distracted) {
        eventCount++;
        armed = false;
        Serial.println("ALERT: 專心一下！");
    }

    // 保守的重新啟用條件：學生要清楚可見，且手機連續 3 秒完全沒有可靠結果。
    bool clearEvidence = !armed && reliable && !phoneDetected;
    updateTimer(clearEvidence, clearStart, now);
    if (!armed && elapsedSince(clearStart, now) >= REARM_MS) {
        armed = true;
        clearStart = 0;
    }
}

void drawOverlay()
{
    uint16_t width = streamConfig.width();
    uint16_t height = streamConfig.height();
    OSD.createBitmap(STREAM_CHANNEL);
    if (personBox.valid) {
        int x1 = personBox.x1 * width, y1 = personBox.y1 * height;
        int x2 = personBox.x2 * width, y2 = personBox.y2 * height;
        OSD.drawRect(STREAM_CHANNEL, x1, y1, x2, y2, 3, OSD_COLOR_CYAN);
        char label[30]; snprintf(label, sizeof(label), "student %d", personBox.score);
        OSD.drawText(STREAM_CHANNEL, x1, max(0, y1 - OSD.getTextHeight(STREAM_CHANNEL)), label, OSD_COLOR_CYAN);
    }
    if (phoneBox.valid) {
        int x1 = phoneBox.x1 * width, y1 = phoneBox.y1 * height;
        int x2 = phoneBox.x2 * width, y2 = phoneBox.y2 * height;
        OSD.drawRect(STREAM_CHANNEL, x1, y1, x2, y2, 3, OSD_COLOR_WHITE);
        char label[34]; snprintf(label, sizeof(label), "cell phone %d", phoneBox.score);
        OSD.drawText(STREAM_CHANNEL, x1, max(0, y1 - OSD.getTextHeight(STREAM_CHANNEL)), label, OSD_COLOR_WHITE);
    }
    if (distracted) {
        OSD.drawText(STREAM_CHANNEL, 24, 36, "FOCUS!", OSD_COLOR_WHITE);
    }
    OSD.update(STREAM_CHANNEL);
}

void processDetections()
{
    unsigned long now = millis();
    std::vector<ObjectDetectionResult> results = detector.getResult();
    Box bestPerson = {0, 0, 0, 0, 0, false};
    Box bestPhone = {0, 0, 0, 0, 0, false};
    bool ambiguous = false;

    for (int i = 0; i < detector.getResultCount(); i++) {
        ObjectDetectionResult item = results[i];
        int type = item.type();
        int score = item.score();
        if (type != PERSON_CLASS && type != PHONE_CLASS) continue;
        if (score >= AMBIGUOUS_SCORE && score < CONFIDENCE_THRESHOLD) ambiguous = true;
        if (score < CONFIDENCE_THRESHOLD) continue;
        Box candidate = {item.xMin(), item.yMin(), item.xMax(), item.yMax(), score, true};
        if (type == PERSON_CLASS && score > bestPerson.score) bestPerson = candidate;
        if (type == PHONE_CLASS && score > bestPhone.score) bestPhone = candidate;
    }

    personBox = bestPerson;
    phoneBox = bestPhone;
    lastResultAt = now;
    updateDecision(now, ambiguous);
    drawOverlay();
}

void writeBoxJson(WiFiClient &client, const Box &box)
{
    if (!box.valid) {
        client.print("null");
        return;
    }
    client.print("{\"x\":"); client.print(box.x1 * 100.0f, 1);
    client.print(",\"y\":"); client.print(box.y1 * 100.0f, 1);
    client.print(",\"w\":"); client.print((box.x2 - box.x1) * 100.0f, 1);
    client.print(",\"h\":"); client.print((box.y2 - box.y1) * 100.0f, 1);
    client.print("}");
}

void sendStatus(WiFiClient &client)
{
    unsigned long now = millis();
    if (now - lastResultAt > RESULT_STALE_MS) reliable = false;
    float ratio = (personBox.valid && phoneBox.valid) ? overlapOfPhone(personBox, phoneBox) : 0.0f;
    client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n");
    client.print("{\"reliable\":"); client.print(reliable ? "true" : "false");
    client.print(",\"personDetected\":"); client.print(personBox.valid ? "true" : "false");
    client.print(",\"phoneDetected\":"); client.print(phoneBox.valid ? "true" : "false");
    client.print(",\"overlap\":"); client.print(overlapNow ? "true" : "false");
    client.print(",\"distracted\":"); client.print(distracted ? "true" : "false");
    client.print(",\"armed\":"); client.print(armed ? "true" : "false");
    client.print(",\"personConfidence\":"); client.print(personBox.score);
    client.print(",\"phoneConfidence\":"); client.print(phoneBox.score);
    client.print(",\"overlapRatio\":"); client.print(ratio, 3);
    client.print(",\"personSeconds\":"); client.print(elapsedSince(personStart, now) / 1000.0f, 1);
    client.print(",\"phoneSeconds\":"); client.print(elapsedSince(phoneStart, now) / 1000.0f, 1);
    client.print(",\"overlapSeconds\":"); client.print(elapsedSince(overlapStart, now) / 1000.0f, 1);
    client.print(",\"clearSeconds\":"); client.print(elapsedSince(clearStart, now) / 1000.0f, 1);
    client.print(",\"eventCount\":"); client.print(eventCount);
    client.print(",\"boxes\":{\"person\":"); writeBoxJson(client, personBox);
    client.print(",\"phone\":"); writeBoxJson(client, phoneBox);
    client.print("}}");
}

void handleStatusClient()
{
    WiFiClient client = statusServer.available();
    if (!client) return;
    unsigned long deadline = millis() + 250;
    String firstLine = "";
    while (client.connected() && millis() < deadline) {
        if (!client.available()) { delay(1); continue; }
        char c = client.read();
        if (c == '\n') break;
        if (c != '\r' && firstLine.length() < 100) firstLine += c;
    }
    while (client.available()) client.read();
    if (firstLine.startsWith("GET /status")) sendStatus(client);
    else client.print("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
    delay(1);
    client.stop();
}

void setup()
{
    Serial.begin(115200);
    int wifiStatus = WL_IDLE_STATUS;
    while (wifiStatus != WL_CONNECTED) {
        Serial.print("Connecting to Wi-Fi: "); Serial.println(ssid);
        wifiStatus = WiFi.begin(ssid, pass);
        delay(2000);
    }

    streamConfig.setBitrate(2 * 1024 * 1024);
    Camera.configVideoChannel(STREAM_CHANNEL, streamConfig);
    Camera.configVideoChannel(NN_CHANNEL, nnConfig);
    Camera.videoInit();

    rtsp.configVideo(streamConfig);
    rtsp.begin();

    detector.configVideo(nnConfig);
    detector.modelSelect(OBJECT_DETECTION, DEFAULT_YOLOV4TINY, NA_MODEL, NA_MODEL);
    detector.begin();

    streamLink.registerInput(Camera.getStream(STREAM_CHANNEL));
    streamLink.registerOutput(rtsp);
    if (streamLink.begin() != 0) Serial.println("RTSP StreamIO start failed");
    Camera.channelBegin(STREAM_CHANNEL);

    nnLink.registerInput(Camera.getStream(NN_CHANNEL));
    nnLink.setStackSize();
    nnLink.setTaskPriority();
    nnLink.registerOutput(detector);
    if (nnLink.begin() != 0) Serial.println("YOLO StreamIO start failed");
    Camera.channelBegin(NN_CHANNEL);

    OSD.configVideo(STREAM_CHANNEL, streamConfig);
    OSD.begin();
    statusServer.begin();

    Serial.print("AMB82 IP: "); Serial.println(WiFi.localIP());
    Serial.print("RTSP: rtsp://"); Serial.print(WiFi.localIP()); Serial.print(":"); Serial.println(rtsp.getPort());
    Serial.print("Status: http://"); Serial.print(WiFi.localIP()); Serial.println(":8080/status");
}

void loop()
{
    processDetections();
    handleStatusClient();
    delay(80);
}
