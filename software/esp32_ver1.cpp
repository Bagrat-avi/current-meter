#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <math.h>

const char* AP_SSID = "ESP32_CurrentMeter";
const char* AP_PASS = "12345678";

IPAddress localIP(192,168,0,1);
IPAddress gateway(192,168,0,1);
IPAddress subnet(255,255,255,0);

const byte ADS_ADDR = 0x48;
const int SDA_PIN = 21;
const int SCL_PIN = 22;

float SHUNT_OHMS = 200;

const int SAMPLE_RATE = 50;
const int MAX_SAMPLES = SAMPLE_RATE * 60;
const unsigned long PERIOD_US = 1000000UL / SAMPLE_RATE;

unsigned long sampleTime[MAX_SAMPLES];
float current_uA[MAX_SAMPLES];

int sampleCount = 0;
int writeIndex = 0;
float lastCurrent_uA = NAN;

bool adsFound = false;

int PGA = 5;

const float PGA_FS[6] = {
  6.144, 4.096, 2.048, 1.024, 0.512, 0.256
};

WebServer server(80);

void writeReg(byte reg, uint16_t value) {
  Wire.beginTransmission(ADS_ADDR);
  Wire.write(reg);
  Wire.write(value >> 8);
  Wire.write(value & 0xFF);
  Wire.endTransmission();
}

uint16_t readReg(byte reg) {
  Wire.beginTransmission(ADS_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(ADS_ADDR, 2);

  if (Wire.available() < 2) return 0;

  byte hi = Wire.read();
  byte lo = Wire.read();
  return ((uint16_t)hi << 8) | lo;
}

bool checkADS() {
  Wire.beginTransmission(ADS_ADDR);
  return Wire.endTransmission() == 0;
}

int16_t readADS(int pga) {

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
    if (millis() - start > 10) return 0;
    delay(1);
  }

  return (int16_t)readReg(0x00);
}

float measureCurrent() {

  if (!adsFound || SHUNT_OHMS <= 0) return NAN;

  int pgaUsed = PGA;
  int raw = readADS(pgaUsed);

  float voltage = raw * PGA_FS[pgaUsed] / 32768.0;

  float current = fabs(voltage) / SHUNT_OHMS;

  float current_uA = current * 1000000.0;

  if (abs(raw) > 30000 && PGA > 0) PGA--;
  if (abs(raw) < 4000 && PGA < 5) PGA++;

  return current_uA;
}

void saveSample() {

  float I = measureCurrent();

  sampleTime[writeIndex] = millis();

  current_uA[writeIndex] = I;

  if (!isnan(I)) lastCurrent_uA = I;

  writeIndex++;

  if (writeIndex >= MAX_SAMPLES) writeIndex = 0;

  if (sampleCount < MAX_SAMPLES) sampleCount++;
}

const char PAGE[] = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Измерение тока</title>

<style>
body{font-family:Arial;margin:20px;background:#f3f3f3}
.box{background:white;padding:15px;margin-bottom:15px;border-radius:10px}
#current{font-size:36px;font-weight:bold}
canvas{width:100%;max-width:1200px;height:450px;border:1px solid #999;background:white}
.table{max-height:450px;overflow:auto;background:white}
table{border-collapse:collapse;width:100%}
th,td{border:1px solid #ccc;padding:5px;text-align:center}
th{background:#eee}
</style>
</head>

<body>

<div class="box">
<h1>Измерение тока</h1>

<div>Состояние: <span id="status">---</span></div>

<div id="current">---</div>

<div>Диапазон: 0.1–100 µA</div>
<div>Частота: 50 Гц</div>
<div>Длительность: 60 с</div>
<div>Количество точек: <span id="count">0</span></div>
</div>

<div class="box">
<h2>График I(t)</h2>
<canvas id="graph" width="1200" height="450"></canvas>
</div>

<div class="box table">
<h2>Измерения</h2>

<table>
<tr>
<th>Время МСК</th>
<th>t, с</th>
<th>I, µA</th>
</tr>

<tbody id="tableBody"></tbody>
</table>

</div>

<script>

function drawGraph(data){

let c=document.getElementById("graph");
let ctx=c.getContext("2d");

ctx.clearRect(0,0,c.width,c.height);

if(data.t.length<2)return;

let L=70,R=20,T=20,B=50;
let W=c.width-L-R;
let H=c.height-T-B;

let t0=data.t[0];
let t1=data.t[data.t.length-1];

if(t1==t0)t1=t0+1;

let min=Infinity;
let max=-Infinity;

for(let i=0;i<data.i.length;i++){
    if(data.i[i]!==null){
        min=Math.min(min,data.i[i]);
        max=Math.max(max,data.i[i]);
    }
}

if(!isFinite(min)){min=0;max=1;}
if(min==max)max=min+1;

let d=(max-min)*0.1;
min=Math.max(0,min-d);
max=max+d;

ctx.beginPath();
ctx.moveTo(L,T);
ctx.lineTo(L,c.height-B);
ctx.lineTo(c.width-R,c.height-B);
ctx.stroke();

ctx.font="14px Arial";
ctx.fillText("I, µA",10,20);
ctx.fillText("t, с",c.width-50,c.height-15);

ctx.beginPath();

for(let i=0;i<data.t.length;i++){

    if(data.i[i]===null)continue;
    let x=L+(data.t[i]-t0)/(t1-t0)*W;

    let y=T+(1-(data.i[i]-min)/(max-min))*H;

    if(i==0)ctx.moveTo(x,y);
    else ctx.lineTo(x,y);
}

ctx.strokeStyle="blue";
ctx.stroke();

ctx.fillStyle="black";
ctx.fillText(max.toFixed(3)+" µA",5,T+5);
ctx.fillText(min.toFixed(3)+" µA",5,c.height-B);

}


function updateTable(data){

let body=document.getElementById("tableBody");

body.innerHTML="";

let start=Math.max(0,data.t.length-50);

let latest=data.t[data.t.length-1];

let browserNow=Date.now();

let startEpoch=browserNow-latest;

for(let i=start;i<data.t.length;i++){

    if(data.i[i]===null)continue;

    let row=body.insertRow();

    let d=new Date(startEpoch+data.t[i]);

    row.insertCell(0).textContent=
        d.toLocaleTimeString(
            "ru-RU",
            {timeZone:"Europe/Moscow"}
        );

    row.insertCell(1).textContent=
        ((data.t[i]-data.t[0])/1000).toFixed(3);

    row.insertCell(2).textContent=
        data.i[i].toFixed(4);
}

}


async function update(){

try{

    let r=await fetch("/api",{cache:"no-store"});
    let data=await r.json();

    document.getElementById("status").textContent=data.status;
    document.getElementById("count").textContent=data.t.length;

    if(data.current===null){
        document.getElementById("current").textContent="---";
    }else{
        document.getElementById("current").textContent=
            data.current.toFixed(4)+" µA";
    }

    drawGraph(data);
    updateTable(data);

}catch(e){

    document.getElementById("status").textContent=
        "Нет соединения с ESP32";

}

}

setInterval(update,1000);
update();

</script>

</body>
</html>
)rawliteral";

void handleRoot() {
  server.send(200,"text/html; charset=utf-8",PAGE);
}

void handleAPI() {

  String json;
  json.reserve(80000);

  json += "{\"status\":\"";

  if (!adsFound) json += "ADS1115 не найден";
  else if (SHUNT_OHMS <= 0) json += "Введите сопротивление шунта";
  else json += "Измерение работает";

  json += "\",\"current\":";

  if (isnan(lastCurrent_uA)) json += "null";
  else json += String(lastCurrent_uA,6);

  json += ",\"t\":[";

  int first = sampleCount < MAX_SAMPLES ? 0 : writeIndex;

  for (int n=0;n<sampleCount;n++) {
    int i=(first+n)%MAX_SAMPLES;
    if(n)json+=",";
    json+=String(sampleTime[i]);
  }

  json += "],\"i\":[";

  for (int n=0;n<sampleCount;n++) {
    int i=(first+n)%MAX_SAMPLES;
    if(n)json+=",";
    if(isnan(current_uA[i]))json+="null";
    else json+=String(current_uA[i],6);
  }

  json += "]}";

  server.sendHeader("Cache-Control","no-store");
  server.send(200,"application/json",json);
}

void setup() {

  Serial.begin(115200);

  Wire.begin(SDA_PIN,SCL_PIN);
  adsFound=checkADS();

  Serial.println(adsFound?"ADS1115 найден":"ADS1115 НЕ найден");

  WiFi.mode(WIFI_AP);

  WiFi.softAPConfig(localIP,gateway,subnet);

  WiFi.softAP(AP_SSID,AP_PASS);

  Serial.print("SSID: ");
  Serial.println(AP_SSID);

  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());

  server.on("/",handleRoot);
  server.on("/api",handleAPI);

  server.begin();

  Serial.println("HTTP server started");
}

void loop() {

  server.handleClient();

  static unsigned long nextSample=micros();

  unsigned long now=micros();

  if((long)(now-nextSample)>=0){

    nextSample+=PERIOD_US;

    saveSample();
  }
}
