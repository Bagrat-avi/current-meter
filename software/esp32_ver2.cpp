
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"


// -------------------- Wi-Fi и сервер --------------------

const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

const char* SERVER_URL =
    "http://192.168.0.250:5000/measurement";


// -------------------- ADS1115 --------------------

const byte ADS_ADDR = 0x48;

const int SDA_PIN = 21;
const int SCL_PIN = 22;


// Пока оставляем значение из твоего исходного кода.
// Перед реальными измерениями необходимо проверить R шунта.
float SHUNT_OHMS = 200.0f;

int PGA = 5;

const float PGA_FS[6] = {
    6.144, 4.096, 2.048, 1.024, 0.512, 0.256
};

bool adsFound = false;


// -------------------- Частота измерений --------------------

const int SAMPLE_RATE = 50;

const int MAX_SAMPLES = SAMPLE_RATE * 60;

const int BATCH_SIZE = 50;

const unsigned long PERIOD_US =
    1000000UL / SAMPLE_RATE;


// -------------------- Очередь измерений --------------------

struct Sample {
    uint32_t time_ms;
    float current_uA;
};

QueueHandle_t sampleQueue = nullptr;

volatile uint32_t droppedSamples = 0;


// -------------------- ADS1115: запись регистра --------------------

void writeReg(byte reg, uint16_t value) {

    Wire.beginTransmission(ADS_ADDR);

    Wire.write(reg);
    Wire.write(value >> 8);
    Wire.write(value & 0xFF);

    Wire.endTransmission();
}


// -------------------- ADS1115: чтение регистра --------------------

uint16_t readReg(byte reg) {

    Wire.beginTransmission(ADS_ADDR);

    Wire.write(reg);

    if (Wire.endTransmission(false) != 0) {
        return 0;
    }

    if (Wire.requestFrom((uint8_t)ADS_ADDR, (uint8_t)2) != 2) {
        return 0;
    }

    if (Wire.available() < 2) {
        return 0;
    }

    byte hi = Wire.read();
    byte lo = Wire.read();

    return ((uint16_t)hi << 8) | lo;
}


// -------------------- Проверка ADS1115 --------------------

bool checkADS() {

    Wire.beginTransmission(ADS_ADDR);

    return Wire.endTransmission() == 0;
}


// -------------------- Одно измерение ADS1115 --------------------

bool readADS(int pga, int16_t &result) {

    uint16_t config =
        0x8000 |
        (0b000 << 12) |
        ((uint16_t)pga << 9) |
        (1 << 8) |
        (0b101 << 5) |
        0x0003;

    writeReg(0x01, config);

    unsigned long start = millis();

    while (!(readReg(0x01) & 0x8000)) {

        if (millis() - start > 20) {
            return false;
        }

        delay(1);
    }

    result = (int16_t)readReg(0x00);

    return true;
}


// -------------------- Расчёт тока --------------------

float measureCurrent() {

    if (!adsFound || SHUNT_OHMS <= 0) {
        return NAN;
    }

    int pgaUsed = PGA;

    int16_t raw;

    if (!readADS(pgaUsed, raw)) {
        return NAN;
    }

    float voltage =
        raw * PGA_FS[pgaUsed] / 32768.0f;

    float current =
        fabsf(voltage) / SHUNT_OHMS;

    float current_uA =
        current * 1000000.0f;

    if ((raw > 30000 || raw < -30000) && PGA > 0) {
        PGA--;
    }

    if (raw > -4000 && raw < 4000 && PGA < 5) {
        PGA++;
    }

    return current_uA;
}


// -------------------- Сохранение измерения в очередь --------------------

void saveSample() {

    float current = measureCurrent();

    if (!isfinite(current)) {
        return;
    }

    Sample sample;

    sample.time_ms = millis();
    sample.current_uA = current;

    if (xQueueSend(sampleQueue, &sample, 0) != pdTRUE) {

        droppedSamples++;

        if (droppedSamples % 50 == 0) {
            Serial.printf(
                "Потеряно измерений: %lu\n",
                (unsigned long)droppedSamples
            );
        }
    }
}


// -------------------- Формирование JSON --------------------

String createJSON(Sample batch[], size_t count) {

    String json;

    json.reserve(40 + count * 55);

    json = "{\"samples\":[";

    for (size_t i = 0; i < count; i++) {

        if (i > 0) {
            json += ",";
        }

        json += "{\"time_ms\":";
        json += String(batch[i].time_ms);

        json += ",\"current_uA\":";
        json += String(batch[i].current_uA, 6);

        json += "}";
    }

    json += "]}";

    return json;
}


// -------------------- Отправка данных на Ubuntu --------------------

void senderTask(void *parameter) {

    Sample batch[BATCH_SIZE];

    bool wasConnected = false;

    while (true) {

        if (WiFi.status() != WL_CONNECTED) {

            if (wasConnected) {
                Serial.println("Wi-Fi отключился");
            }

            wasConnected = false;

            WiFi.reconnect();

            vTaskDelay(pdMS_TO_TICKS(2000));

            continue;
        }

        if (!wasConnected) {

            Serial.println("Wi-Fi подключен");

            Serial.print("IP ESP32: ");
            Serial.println(WiFi.localIP());

            wasConnected = true;
        }

        // Забираем до 50 измерений из очереди.

        size_t count = 0;

        while (count < BATCH_SIZE) {

            TickType_t waitTime;

            if (count == 0) {
                waitTime = pdMS_TO_TICKS(1000);
            } else {
                waitTime = pdMS_TO_TICKS(100);
            }

            if (xQueueReceive(
                    sampleQueue,
                    &batch[count],
                    waitTime
                ) == pdTRUE) {

                count++;

            } else if (count > 0) {

                break;
            }
        }

        if (count == 0) {
            continue;
        }

        String payload = createJSON(batch, count);

        bool delivered = false;

        // При ошибке сохраняем пакет в памяти задачи
        // и повторяем отправку.

        while (!delivered) {

            if (WiFi.status() != WL_CONNECTED) {

                WiFi.reconnect();

                vTaskDelay(pdMS_TO_TICKS(2000));

                continue;
            }

            WiFiClient client;

            HTTPClient http;

            http.setTimeout(5000);

            if (!http.begin(client, SERVER_URL)) {

                Serial.println("Ошибка подключения HTTP");

                vTaskDelay(pdMS_TO_TICKS(2000));

                continue;
            }

            http.addHeader(
                "Content-Type",
                "application/json"
            );

            int responseCode = http.POST(payload);

            String response = "";

            if (responseCode > 0) {
                response = http.getString();
            }

            http.end();

            if (responseCode >= 200 && responseCode < 300) {

                Serial.printf(
                    "Отправлено измерений: %u\n",
                    (unsigned int)count
                );

                Serial.printf(
                    "Ответ сервера: %s\n",
                    response.c_str()
                );

                delivered = true;

            } else {

                Serial.printf(
                    "Ошибка HTTP: %d\n",
                    responseCode
                );

                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        }
    }
}


// -------------------- SETUP --------------------

void setup() {

    Serial.begin(115200);

    delay(500);

    // Запускаем I2C.

    Wire.begin(SDA_PIN, SCL_PIN);

    // Проверяем ADS1115.

    adsFound = checkADS();

    if (adsFound) {
        Serial.println("ADS1115 найден");
    } else {
        Serial.println("ADS1115 НЕ найден");
    }

    // Создаём очередь на 3000 измерений.

    sampleQueue = xQueueCreate(
        MAX_SAMPLES,
        sizeof(Sample)
    );

    if (sampleQueue == nullptr) {

        Serial.println("Ошибка создания очереди");

        while (true) {
            delay(1000);
        }
    }

    // Подключаемся к роутеру.

    WiFi.mode(WIFI_STA);

    WiFi.setAutoReconnect(true);

    WiFi.begin(WIFI_SSID, WIFI_PASS);

    Serial.println("Подключение к Wi-Fi...");

    // Запускаем отдельную задачу передачи данных.

    BaseType_t result = xTaskCreate(
        senderTask,
        "senderTask",
        8192,
        nullptr,
        1,
        nullptr
    );

    if (result != pdPASS) {

        Serial.println("Ошибка запуска задачи передачи");

        while (true) {
            delay(1000);
        }
    }

    Serial.println("Измерительная система запущена");
}


// -------------------- LOOP --------------------

void loop() {

    if (!adsFound) {
        delay(1000);
        return;
    }

    static unsigned long nextSample = micros();

    unsigned long now = micros();

    if ((long)(now - nextSample) >= 0) {

        nextSample += PERIOD_US;

        saveSample();

    } else {

        delayMicroseconds(200);
    }
}
