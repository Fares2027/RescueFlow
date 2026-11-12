#pragma once
#include <Arduino.h>

enum class Phase : uint8_t {
  EW_GREEN = 0, EW_YELLOW = 1, ALL_RED_EW_TO_NS = 2,
  NS_GREEN = 3, NS_YELLOW = 4, ALL_RED_NS_TO_EW = 5
};
enum class Approach : uint8_t { NONE, EW, NS };
// An explicit mode avoids leaving the controller frozen after sync loss.
enum class ControlMode : uint8_t {
  STANDALONE, SUMO_TAKEOVER, SUMO_SYNC, FALLBACK_ALL_RED, SUMO_WAITING
};
// User's iPhone choice. AUTO is the boot default; selections aren't persisted.
enum class ModeSelection : uint8_t { AUTO, STANDALONE, SUMO };

class TrafficController {
public:
  void begin(uint32_t now);
  void update(uint32_t now);
  void syncExternalPhase(Phase next, uint32_t now);
  bool setModeSelection(ModeSelection next, uint32_t now);
  ModeSelection modeSelection() const { return selection_; }
  const char* modeSelectionName() const;
  bool acceptsExternalSync() const { return selection_ != ModeSelection::STANDALONE; }
  bool externalMode() const { return mode_ == ControlMode::SUMO_TAKEOVER || mode_ == ControlMode::SUMO_SYNC || mode_ == ControlMode::SUMO_WAITING; }
  bool request(Approach approach, uint32_t now);
  bool clear(uint32_t now);
  Phase phase() const { return phase_; }
  Approach activeApproach() const { return active_; }
  Approach pendingApproach() const { return pending_; }
  bool priorityActive() const { return priority_; }
  bool requestPending() const { return pending_ != Approach::NONE; }
  const char* phaseName() const;
  const char* statusName() const;
  uint32_t phaseSeconds(uint32_t now) const { return (now - phaseStart_) / 1000UL; }
private:
  void setPhase(Phase next, uint32_t now);
  void setOutputs();
  void finishPriority(uint32_t now);
  Phase phase_ = Phase::ALL_RED_NS_TO_EW;
  uint32_t phaseStart_ = 0;
  uint32_t priorityStart_ = 0;
  Approach pending_ = Approach::NONE;
  Approach active_ = Approach::NONE;
  bool priority_ = false;
  bool clearing_ = false;
  ControlMode mode_ = ControlMode::STANDALONE;
  ModeSelection selection_ = ModeSelection::AUTO;
  uint32_t lastExternalUpdate_ = 0;
  uint32_t transitionStart_ = 0;
  Phase requestedExternalPhase_ = Phase::ALL_RED_EW_TO_NS;
};
