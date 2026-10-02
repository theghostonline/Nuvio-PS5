#ifndef EVO_I_STATE_MACHINE_HPP
#define EVO_I_STATE_MACHINE_HPP

#include <cstddef>

namespace evo {

/**
 * @brief Non-template interface for runtime state machine introspection and driving.
 *
 * Allows controllers, managers, and developer tools to interact with state machines
 * uniformly regardless of the concrete enum types used for states and events.
 */
class IStateMachine {
public:
    virtual ~IStateMachine() = default;

    /**
     * @brief Advance temporal state logic (timers, timeouts, animated transitions).
     * @param deltaMs Elapsed time in milliseconds since the last frame.
     */
    virtual void update(double deltaMs) = 0;

    /**
     * @brief Human-readable name of the current active state.
     */
    virtual const char* getCurrentStateName() const = 0;

    /**
     * @brief Integer identifier of the current active state.
     */
    virtual int getCurrentStateId() const = 0;

    /**
     * @brief Query if the machine is currently in a given state.
     */
    virtual bool isInState(int stateId) const = 0;

    /**
     * @brief Dispatch an event by integer identifier.
     * @return true if a valid transition occurred, false if discarded or blocked by a guard.
     */
    virtual bool dispatchEvent(int eventId) = 0;

    /**
     * @brief Reset the state machine back to its initial state.
     */
    virtual void reset() = 0;

    /**
     * @brief Total number of successful transitions executed by this machine.
     */
    virtual size_t getTransitionCount() const = 0;
};

} // namespace evo

#endif // EVO_I_STATE_MACHINE_HPP
