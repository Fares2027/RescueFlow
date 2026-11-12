#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "pin_config.h"
#include "traffic_control.h"

// Demonstration only: local ESP32 access point, not a public-road control system.
static const char* AP_SSID = "RescueFlow-Demo";
static const char* AP_PASSWORD = "RescueFlow2026"; // Change before demonstrations.
static const char* API_KEY = "1234"; // Change before demonstrations.
static constexpr uint32_t EW_GREEN_MS = 42000;
static constexpr uint32_t NS_GREEN_MS = 42000;
static constexpr uint32_t YELLOW_MS = 3000;
static constexpr uint32_t ALL_RED_MS = 2000;
static constexpr uint32_t MAX_PRIORITY_MS = 20000;
static constexpr uint32_t MIN_GREEN_MS = 5000;
// If the SUMO heartbeat stops, return to standalone after an all-red clearance.
static constexpr uint32_t SYNC_TIMEOUT_MS = 3500;

TrafficController traffic;
WebServer server(80);
// The phone only requests a fixed SUMO action. Windows executes it via a local agent.
static bool testRunRequested = false;
static bool laptopSeen = false;
static uint32_t laptopLastSeen = 0;
static String laptopState = "OFFLINE";
static constexpr uint32_t LAPTOP_TIMEOUT_MS = 5000;
static bool laptopOnline() {
  return laptopSeen && (uint32_t)(millis() - laptopLastSeen) < LAPTOP_TIMEOUT_MS;
}


static const char* approachName(Approach a) {
  switch (a) {
    case Approach::EW: return "EW";
    case Approach::NS: return "NS";
    default: return "NONE";
  }
}

void TrafficController::setOutputs() {
    const bool ewGreen = phase_ == Phase::EW_GREEN;
    const bool ewYellow = phase_ == Phase::EW_YELLOW;
    const bool nsGreen = phase_ == Phase::NS_GREEN;
    const bool nsYellow = phase_ == Phase::NS_YELLOW;

    // Active-Low: LOW = ON, HIGH = OFF
    digitalWrite(EW_GREEN_PIN, (ewGreen && !nsGreen) ? LOW : HIGH);
    digitalWrite(EW_YELLOW_PIN, ewYellow ? LOW : HIGH);
    digitalWrite(EW_RED_PIN, (!ewGreen && !ewYellow) ? LOW : HIGH);

    digitalWrite(NS_GREEN_PIN, (nsGreen && !ewGreen) ? LOW : HIGH);
    digitalWrite(NS_YELLOW_PIN, nsYellow ? LOW : HIGH);
    digitalWrite(NS_RED_PIN, (!nsGreen && !nsYellow) ? LOW : HIGH);
}

void TrafficController::setPhase(Phase next, uint32_t now) {
  phase_ = next;
  phaseStart_ = now;
  setOutputs();
  Serial.printf("[%lu ms] PHASE %u: %s\n", (unsigned long)now,
                (unsigned)phase_, phaseName());
}

void TrafficController::begin(uint32_t now) {
  const uint8_t pins[] = {EW_RED_PIN, EW_YELLOW_PIN, EW_GREEN_PIN,
                          NS_RED_PIN, NS_YELLOW_PIN, NS_GREEN_PIN};
  for (uint8_t pin : pins) {
    digitalWrite(pin, HIGH); // OFF before GPIO becomes an output (Active-Low)
    pinMode(pin, OUTPUT);
  }
  setPhase(Phase::ALL_RED_NS_TO_EW, now);
}

bool TrafficController::setModeSelection(ModeSelection next, uint32_t now) {
  if (selection_ == next) return false;
  selection_ = next;
  // Never switch directly between conflicting active directions.
  pending_ = Approach::NONE;
  active_ = Approach::NONE;
  priority_ = false;
  clearing_ = false;
  mode_ = ControlMode::FALLBACK_ALL_RED;
  transitionStart_ = now;
  setPhase(Phase::ALL_RED_EW_TO_NS, now);
  Serial.printf("Control selection changed to %s; all-red clearance\n", modeSelectionName());
  return true;
}

const char* TrafficController::modeSelectionName() const {
  switch (selection_) {
    case ModeSelection::AUTO: return "AUTO";
    case ModeSelection::STANDALONE: return "STANDALONE";
    case ModeSelection::SUMO: return "SUMO";
  }
  return "UNKNOWN";
}

void TrafficController::syncExternalPhase(Phase next, uint32_t now) {
  if (!acceptsExternalSync()) return;
  lastExternalUpdate_ = now;
  requestedExternalPhase_ = next;
  if (mode_ == ControlMode::STANDALONE || mode_ == ControlMode::SUMO_WAITING) {
    // Clear conflicting local indications before accepting SUMO phases.
    pending_ = Approach::NONE;
    active_ = Approach::NONE;
    priority_ = false;
    clearing_ = false;
    mode_ = ControlMode::SUMO_TAKEOVER;
    transitionStart_ = now;
    setPhase(Phase::ALL_RED_EW_TO_NS, now);
    Serial.println("SUMO takeover: all red clearance");
    return;
  }
  if (mode_ == ControlMode::FALLBACK_ALL_RED) {
    // Already clearing. Preserve the start time rather than extending the
    // clearance every time Python sends a heartbeat.
    mode_ = ControlMode::SUMO_TAKEOVER;
    return;
  }
  if (mode_ == ControlMode::SUMO_SYNC && phase_ != next) {
    setPhase(next, now);
  }
  // SUMO_TAKEOVER stores the latest phase until all-red clearance completes.
}

const char* TrafficController::phaseName() const {
  switch (phase_) {
    case Phase::EW_GREEN: return "EW Green";
    case Phase::EW_YELLOW: return "EW Yellow";
    case Phase::ALL_RED_EW_TO_NS: return "All Red EW to NS";
    case Phase::NS_GREEN: return "NS Green";
    case Phase::NS_YELLOW: return "NS Yellow";
    case Phase::ALL_RED_NS_TO_EW: return "All Red NS to EW";
  }
  return "Unknown";
}

const char* TrafficController::statusName() const {
  if (mode_ == ControlMode::SUMO_TAKEOVER) return "SUMO_TAKEOVER";
  if (mode_ == ControlMode::SUMO_SYNC) return "SUMO_SYNC";
  if (mode_ == ControlMode::FALLBACK_ALL_RED) return "MODE_TRANSITION";
  if (mode_ == ControlMode::SUMO_WAITING) return "SUMO_WAITING";
  if (clearing_) return "CLEARING";
  if (priority_) return "PRIORITY_ACTIVE";
  if (pending_ != Approach::NONE) return "REQUEST_PENDING";
  return "NORMAL";
}

bool TrafficController::request(Approach approach, uint32_t now) {
  if (mode_ != ControlMode::STANDALONE || approach == Approach::NONE ||
      pending_ != Approach::NONE || priority_ || clearing_)
    return false;
  pending_ = approach;
  Serial.printf("[%lu ms] REQUEST %s\n", (unsigned long)now, approachName(approach));
  update(now);
  return true;
}

void TrafficController::finishPriority(uint32_t now) {
  priority_ = false;
  clearing_ = false;
  active_ = Approach::NONE;
  pending_ = Approach::NONE;
  // Resume normal cycle from the beginning of the current green phase.
  setPhase(phase_, now);
  Serial.println("PRIORITY ENDED; NORMAL CYCLE RESUMED");
}

bool TrafficController::clear(uint32_t now) {
  if (mode_ != ControlMode::STANDALONE || !priority_ || clearing_) return false;
  // Operator confirms passage; do not immediately cut green before minimum hold.
  clearing_ = true;
  update(now);
  return true;
}

void TrafficController::update(uint32_t now) {
  // Heartbeats stop if Wi-Fi disconnects, Python exits, or SUMO stalls.
  if ((mode_ == ControlMode::SUMO_TAKEOVER || mode_ == ControlMode::SUMO_SYNC) &&
      (uint32_t)(now - lastExternalUpdate_) > SYNC_TIMEOUT_MS) {
    mode_ = ControlMode::FALLBACK_ALL_RED;
    transitionStart_ = now;
    setPhase(Phase::ALL_RED_EW_TO_NS, now);
    Serial.println("SUMO heartbeat lost: all red clearance");
    return;
  }
  if (mode_ == ControlMode::SUMO_TAKEOVER) {
    if ((uint32_t)(now - transitionStart_) >= ALL_RED_MS) {
      mode_ = ControlMode::SUMO_SYNC;
      setPhase(requestedExternalPhase_, now);
      Serial.println("SUMO synchronization active");
    }
    return;
  }
  if (mode_ == ControlMode::SUMO_SYNC) {
    return; // External SUMO phase is authoritative while heartbeats arrive.
  }
  if (mode_ == ControlMode::FALLBACK_ALL_RED) {
    if ((uint32_t)(now - transitionStart_) >= ALL_RED_MS) {
      if (selection_ == ModeSelection::SUMO) {
        mode_ = ControlMode::SUMO_WAITING;
        Serial.println("Manual SUMO mode: waiting at all red for sync");
      } else {
        mode_ = ControlMode::STANDALONE;
        setPhase(Phase::EW_GREEN, now);
        Serial.println("Standalone traffic cycle resumed");
      }
    }
    return;
  }
  if (mode_ == ControlMode::SUMO_WAITING) return;
  uint32_t elapsed = now - phaseStart_;
  if (priority_) {
    if (clearing_ || now - priorityStart_ >= MAX_PRIORITY_MS) {
      if (elapsed >= MIN_GREEN_MS) finishPriority(now);
    }
    return;
  }

  // Pending request for the currently green approach: grant directly.
  if ((phase_ == Phase::EW_GREEN && pending_ == Approach::EW) ||
      (phase_ == Phase::NS_GREEN && pending_ == Approach::NS)) {
    priority_ = true;
    active_ = pending_;
    pending_ = Approach::NONE;
    priorityStart_ = now;
    phaseStart_ = now; // guarantee minimum priority green hold
    Serial.printf("[%lu ms] PRIORITY GRANTED %s\n", (unsigned long)now,
                  approachName(active_));
    return;
  }

  // Opposing green: first preserve minimum green, then transition via yellow and all-red.
  if (phase_ == Phase::EW_GREEN && pending_ == Approach::NS && elapsed >= MIN_GREEN_MS) {
    setPhase(Phase::EW_YELLOW, now);
    return;
  }
  if (phase_ == Phase::NS_GREEN && pending_ == Approach::EW && elapsed >= MIN_GREEN_MS) {
    setPhase(Phase::NS_YELLOW, now);
    return;
  }

  switch (phase_) {
    case Phase::EW_GREEN:
      if (elapsed >= EW_GREEN_MS) setPhase(Phase::EW_YELLOW, now);
      break;
    case Phase::EW_YELLOW:
      if (elapsed >= YELLOW_MS) setPhase(Phase::ALL_RED_EW_TO_NS, now);
      break;
    case Phase::ALL_RED_EW_TO_NS:
      if (elapsed >= ALL_RED_MS) setPhase(Phase::NS_GREEN, now);
      break;
    case Phase::NS_GREEN:
      if (elapsed >= NS_GREEN_MS) setPhase(Phase::NS_YELLOW, now);
      break;
    case Phase::NS_YELLOW:
      if (elapsed >= YELLOW_MS) setPhase(Phase::ALL_RED_NS_TO_EW, now);
      break;
    case Phase::ALL_RED_NS_TO_EW:
      if (elapsed >= ALL_RED_MS) setPhase(Phase::EW_GREEN, now);
      break;
  }
}

static const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RescueFlow | Emergency Demo</title><style>
:root{font-family:system-ui,sans-serif;color-scheme:dark}
body{margin:0;background:#101827;color:#f1f5f9}
main{max-width:560px;margin:28px auto;padding:20px}
section{background:#1e293b;padding:22px;border-radius:16px;margin-bottom:16px}
h1{margin:0 0 8px}p{color:#94a3b8}h2{font-size:18px}
button{border:0;border-radius:10px;padding:17px;margin:6px 0;font-size:17px;font-weight:700;cursor:pointer;width:100%}
.ew{background:#38bdf8;color:#082f49}.ns{background:#a78bfa;color:#2e1065}
.clear{background:#fbbf24;color:#422006}
.apply{background:#34d399;color:#052e22}
label.choice{display:block;padding:9px 0;color:#e2e8f0}
label.choice input{margin-right:8px;accent-color:#34d399}
button:disabled{opacity:.45;cursor:not-allowed}
strong{color:#f8fafc}small{color:#94a3b8}
#msg{min-height:24px;color:#fbbf24}
</style></head><body><main>
<h1>RescueFlow</h1><p>Local emergency priority demonstration</p>
<section><h2>Intersection status</h2>
<p>Phase: <strong id="phase">Connecting...</strong></p>
<p>State: <strong id="mode">...</strong></p>
<p>Selected control: <strong id="selected">...</strong></p>
<p>Active approach: <strong id="active">...</strong></p>
<p>Pending approach: <strong id="pending">...</strong></p>
<p>Phase elapsed: <strong id="elapsed">...</strong> s</p></section>
<section><h2>Control Mode</h2>
<label class="choice"><input type="radio" name="control" value="auto" checked> Automatic Hybrid (default)</label>
<label class="choice"><input type="radio" name="control" value="standalone"> Standalone — iPhone requests</label>
<label class="choice"><input type="radio" name="control" value="sumo"> SUMO Sync — wait for laptop</label>
<button class="apply" onclick="applyMode()">Apply Mode</button>
<small>Each change uses a 2-second all-red transition. Manual SUMO mode stays all red if the laptop stops sending phases.</small>
</section>
<section><h2>Laptop SUMO + Physical Test</h2>
<p>Laptop launcher: <strong id="laptopOnline">Checking...</strong></p>
<p>SUMO process: <strong id="laptopState">...</strong></p>
<button class="apply" id="startTest" onclick="startTest()">Start SUMO + Physical Test</button>
<button class="clear" id="stopTest" onclick="stopTest()">Stop Test</button>
<small>Keep the laptop launcher running and both devices connected to RescueFlow-Demo Wi-Fi. Pressing Start selects SUMO mode automatically.</small>
</section>
<section><h2>Emergency request</h2>
<button class="ew" id="ew" onclick="send('EW')">Request East–West priority</button>
<button class="ns" id="ns" onclick="send('NS')">Request North–South priority</button>
<button class="clear" id="clear" onclick="clearRequest()">Ambulance Cleared</button>
<p id="msg" role="status"></p><small>For breadboard LEDs only. Never connect to public traffic signals.</small>
</section></main><script>
let key='';
async function post(path,data={}){
  if(!key) key=prompt('Enter demonstration API key:')||'';
  const body=new URLSearchParams({...data,key});
  const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
  const obj=await r.json();
  document.getElementById('msg').textContent=obj.message||obj.error||'';
  if(r.status===403)key='';
  await refresh();
}
async function send(direction){try{await post('/api/request',{direction})}catch(e){document.getElementById('msg').textContent='Connection error'}}
async function clearRequest(){try{await post('/api/clear')}catch(e){document.getElementById('msg').textContent='Connection error'}}
async function startTest(){try{await post('/api/test/start')}catch(e){document.getElementById('msg').textContent='Connection error'}}
async function stopTest(){try{await post('/api/test/stop')}catch(e){document.getElementById('msg').textContent='Connection error'}}
async function applyMode(){
  const chosen=document.querySelector('input[name="control"]:checked');
  if(!chosen)return;
  try{await post('/api/mode',{mode:chosen.value})}
  catch(e){document.getElementById('msg').textContent='Connection error'}
}
let initialized=false;
async function refresh(){try{
  const r=await fetch('/api/status',{cache:'no-store'}),s=await r.json();
  document.getElementById('phase').textContent=s.phase;
  document.getElementById('mode').textContent=s.mode;
  document.getElementById('selected').textContent=s.selection;
  if(!initialized){
    const chosen=document.querySelector('input[name="control"][value="'+s.selection.toLowerCase()+'"]');
    if(chosen)chosen.checked=true;
    initialized=true;
  }
  document.getElementById('active').textContent=s.active;
  document.getElementById('pending').textContent=s.pending;
  document.getElementById('elapsed').textContent=s.phase_elapsed_s;
  document.getElementById('laptopOnline').textContent=s.laptop_online?'CONNECTED':'NOT CONNECTED';
  document.getElementById('laptopState').textContent=s.laptop_state;
  document.getElementById('startTest').disabled=!s.laptop_online||s.test_requested;
  document.getElementById('stopTest').disabled=!s.test_requested;
  document.getElementById('ew').disabled=s.mode!=='NORMAL';
  document.getElementById('ns').disabled=s.mode!=='NORMAL';
  document.getElementById('clear').disabled=s.mode!=='PRIORITY_ACTIVE';
}catch(e){document.getElementById('mode').textContent='DISCONNECTED'}}
refresh();setInterval(refresh,1000);
</script></body></html>
)HTML";

static void jsonResponse(int code, const String& json) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", json);
}
static bool authorized() {
  if (!server.hasArg("key") || server.arg("key") != API_KEY) {
    jsonResponse(403, "{\"error\":\"Invalid demonstration key\"}");
    return false;
  }
  return true;
}
static void status() {
  String json = "{\"phase\":\"";
  json += traffic.phaseName();
  json += "\",\"mode\":\"";
  json += traffic.statusName();
  json += "\",\"selection\":\"";
  json += traffic.modeSelectionName();
  json += "\",\"active\":\"";
  json += approachName(traffic.activeApproach());
  json += "\",\"pending\":\"";
  json += approachName(traffic.pendingApproach());
  json += "\",\"phase_elapsed_s\":";
  json += String(traffic.phaseSeconds(millis()));
  json += ",\"laptop_online\":";
  json += laptopOnline() ? "true" : "false";
  json += ",\"laptop_state\":\"";
  json += laptopOnline() ? laptopState : String("OFFLINE");
  json += "\",\"test_requested\":";
  json += testRunRequested ? "true" : "false";
  json += "}";
  jsonResponse(200, json);
}
static void requestHandler() {
  if (!authorized()) return;
  if (traffic.externalMode()) {
    jsonResponse(409, "{\"error\":\"SUMO controls the signals. Use SUMO emergency traffic for this test.\"}");
    return;
  }
  Approach a = Approach::NONE;
  if (server.arg("direction") == "EW") a = Approach::EW;
  if (server.arg("direction") == "NS") a = Approach::NS;
  if (a == Approach::NONE) {
    jsonResponse(400, "{\"error\":\"Direction must be EW or NS\"}");
    return;
  }
  if (!traffic.request(a, millis())) {
    jsonResponse(409, "{\"error\":\"Another request is already in progress\"}");
    return;
  }
  jsonResponse(202, "{\"message\":\"Emergency request accepted\"}");
}
static void clearHandler() {
  if (!authorized()) return;
  if (traffic.externalMode()) {
    jsonResponse(409, "{\"error\":\"SUMO controls the signals.\"}");
    return;
  }
  if (!traffic.clear(millis())) {
    jsonResponse(409, "{\"error\":\"No active priority to clear\"}");
    return;
  }
  jsonResponse(200, "{\"message\":\"Clearance acknowledged\"}");
}

// iPhone mode override. Only for the local demonstration network.
static void modeHandler() {
  if (!authorized()) return;
  const String value = server.arg("mode");
  ModeSelection next;
  if (value == "auto") next = ModeSelection::AUTO;
  else if (value == "standalone") next = ModeSelection::STANDALONE;
  else if (value == "sumo") next = ModeSelection::SUMO;
  else {
    jsonResponse(400, "{\"error\":\"Mode must be auto, standalone or sumo\"}");
    return;
  }
  // A manual mode selection cancels any laptop-start request.
  testRunRequested = false;
  const bool changed = traffic.setModeSelection(next, millis());
  jsonResponse(200, changed ? "{\"message\":\"Mode selected; all-red transition started\"}"
                            : "{\"message\":\"Mode already selected\"}");
}

// The laptop polls this request. No arbitrary shell command is accepted.
static void laptopControlHandler() {
  if (!authorized()) return;
  jsonResponse(200, testRunRequested ? "{\"run\":true}" : "{\"run\":false}");
}
static void laptopHeartbeatHandler() {
  if (!authorized()) return;
  const String state = server.arg("state");
  if (state != "READY" && state != "STARTING" && state != "RUNNING" &&
      state != "STOPPED" && state != "EXITED" && state != "ERROR") {
    jsonResponse(400, "{\"error\":\"Invalid agent state\"}");
    return;
  }
  laptopState = state; // Strict whitelist avoids introducing raw text into JSON.
  laptopLastSeen = millis();
  laptopSeen = true;
  jsonResponse(200, "{\"ok\":true}");
}
static void testStartHandler() {
  if (!authorized()) return;
  if (!laptopOnline()) {
    jsonResponse(409, "{\"error\":\"Start laptop launcher first\"}");
    return;
  }
  if (testRunRequested) {
    jsonResponse(200, "{\"message\":\"SUMO already requested\"}");
    return;
  }
  // SUMO is authoritative only after the existing safe all-red handoff.
  traffic.setModeSelection(ModeSelection::SUMO, millis());
  testRunRequested = true;
  jsonResponse(202, "{\"message\":\"Starting SUMO on laptop\"}");
}
static void testStopHandler() {
  if (!authorized()) return;
  testRunRequested = false;
  // Immediately start all-red clearance before resuming local control.
  traffic.setModeSelection(ModeSelection::STANDALONE, millis());
  jsonResponse(202, "{\"message\":\"Stopping SUMO; standalone resumes after all-red\"}");
}

// Receives an externally controlled SUMO phase (0..5); authorized POST only.
static void syncHandler() {
  if (!authorized()) return;
  if (!server.hasArg("phase")) {
    jsonResponse(400, "{\"error\":\"Missing phase\"}");
    return;
  }
  const String value = server.arg("phase");
  if (value.length() != 1 || value[0] < '0' || value[0] > '5') {
    jsonResponse(400, "{\"error\":\"Phase must be 0..5\"}");
    return;
  }
  if (!traffic.acceptsExternalSync()) {
    // Acknowledge for backward compatibility with the existing bridge, but
    // explicitly report that the user has overridden Python control.
    jsonResponse(200, "{\"ok\":true,\"ignored\":true}");
    return;
  }
  traffic.syncExternalPhase(static_cast<Phase>(value[0] - '0'), millis());
  jsonResponse(200, "{\"ok\":true,\"ignored\":false}");
}

void setup() {
  Serial.begin(115200);
  traffic.begin(millis());
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) Serial.println("Wi-Fi AP failed");
  Serial.print("Connect to Wi-Fi: "); Serial.println(AP_SSID);
  Serial.print("Open: http://"); Serial.println(WiFi.softAPIP());
  server.on("/", HTTP_GET, [](){server.send_P(200, "text/html", PAGE);});
  server.on("/api/status", HTTP_GET, status);
  server.on("/api/request", HTTP_POST, requestHandler);
  server.on("/api/clear", HTTP_POST, clearHandler);
  server.on("/api/mode", HTTP_POST, modeHandler);
  server.on("/api/test/start", HTTP_POST, testStartHandler);
  server.on("/api/test/stop", HTTP_POST, testStopHandler);
  server.on("/api/laptop/control", HTTP_GET, laptopControlHandler);
  server.on("/api/laptop/heartbeat", HTTP_POST, laptopHeartbeatHandler);
  server.on("/api/sync", HTTP_POST, syncHandler);
  server.onNotFound([](){jsonResponse(404, "{\"error\":\"Not found\"}");});
  server.begin();
}
void loop() {
  const uint32_t now = millis();
  traffic.update(now);
  server.handleClient();
  delay(2);
}
