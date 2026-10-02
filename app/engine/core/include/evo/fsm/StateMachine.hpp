#ifndef EVO_STATE_MACHINE_HPP
#define EVO_STATE_MACHINE_HPP

#include "evo/fsm/IStateMachine.hpp"
#include <vector>
#include <string>
#include <functional>
#include <cstdio>
#include <algorithm>

namespace evo {

/**
 * @brief Strongly typed finite state machine supporting guarded transitions,
 * entry/exit/update hooks, and debug observation.
 *
 * Designed specifically for PS5 homebrew constraints: zero heap allocations
 * on event dispatch, no <iostream>, safe static initialization.
 *
 * @tparam TState Enum class representing discrete machine states.
 * @tparam TEvent Enum class representing input triggers/events.
 */
template <typename TState, typename TEvent>
class StateMachine : public IStateMachine {
public:
    using GuardPredicate = std::function<bool()>;
    using ActionCallback = std::function<void()>;
    using UpdateCallback = std::function<void(double deltaMs)>;
    using TransitionObserver = std::function<void(TState from, TEvent event, TState to)>;

    struct StateNode {
        TState id;
        const char* name = "Unknown";
        ActionCallback onEnter;
        ActionCallback onExit;
        UpdateCallback onUpdate;
    };

    struct Transition {
        TState from;
        TEvent event;
        TState to;
        GuardPredicate guard;
        ActionCallback action;
    };

    explicit StateMachine(TState initialState, const char* name = "StateMachine")
        : m_name(name ? name : "StateMachine")
        , m_initialState(initialState)
        , m_currentState(initialState)
    {
    }

    ~StateMachine() override = default;

    /**
     * @brief Configure state hooks.
     */
    StateMachine& addState(TState state,
                           const char* name,
                           ActionCallback onEnter = nullptr,
                           ActionCallback onExit = nullptr,
                           UpdateCallback onUpdate = nullptr) {
        for (auto& s : m_states) {
            if (s.id == state) {
                s.name = name;
                s.onEnter = std::move(onEnter);
                s.onExit = std::move(onExit);
                s.onUpdate = std::move(onUpdate);
                return *this;
            }
        }
        StateNode node;
        node.id = state;
        node.name = name ? name : "Unknown";
        node.onEnter = std::move(onEnter);
        node.onExit = std::move(onExit);
        node.onUpdate = std::move(onUpdate);
        m_states.push_back(std::move(node));
        return *this;
    }

    /**
     * @brief Register a transition rule.
     */
    StateMachine& addTransition(TState from,
                                TEvent event,
                                TState to,
                                GuardPredicate guard = nullptr,
                                ActionCallback action = nullptr) {
        Transition t;
        t.from = from;
        t.event = event;
        t.to = to;
        t.guard = std::move(guard);
        t.action = std::move(action);
        m_transitions.push_back(std::move(t));
        return *this;
    }

    /**
     * @brief Attach an observer callback invoked upon every successful state transition.
     */
    void setTransitionObserver(TransitionObserver observer) {
        m_observer = std::move(observer);
    }

    /**
     * @brief Post a typed event to trigger state transitions.
     * @return true if a transition was fired, false otherwise.
     */
    bool postEvent(TEvent event) {
        for (const auto& t : m_transitions) {
            if (t.from == m_currentState && t.event == event) {
                if (t.guard && !t.guard()) {
                    continue; // Guard rejected transition
                }

                TState oldState = m_currentState;
                TState newState = t.to;

                // 1. Exit old state
                if (StateNode* oldNode = findState(oldState)) {
                    if (oldNode->onExit) {
                        oldNode->onExit();
                    }
                }

                // 2. Execute transition action
                if (t.action) {
                    t.action();
                }

                // 3. Update active state
                m_currentState = newState;
                m_transitionCount++;

                // 4. Notify observer
                if (m_observer) {
                    m_observer(oldState, event, newState);
                }

                // 5. Enter new state
                if (StateNode* newNode = findState(newState)) {
                    if (newNode->onEnter) {
                        newNode->onEnter();
                    }
                }

                return true;
            }
        }
        return false;
    }

    /**
     * @brief Check whether an event can be handled from the current state.
     */
    bool canHandle(TEvent event) const {
        for (const auto& t : m_transitions) {
            if (t.from == m_currentState && t.event == event) {
                if (!t.guard || t.guard()) {
                    return true;
                }
            }
        }
        return false;
    }

    TState getCurrentState() const {
        return m_currentState;
    }

    /**
     * @brief Direct state override (used for testing or forced resets).
     */
    void setState(TState state, bool triggerCallbacks = true) {
        if (triggerCallbacks) {
            if (StateNode* oldNode = findState(m_currentState)) {
                if (oldNode->onExit) oldNode->onExit();
            }
        }
        m_currentState = state;
        if (triggerCallbacks) {
            if (StateNode* newNode = findState(m_currentState)) {
                if (newNode->onEnter) newNode->onEnter();
            }
        }
    }

    // --- IStateMachine Implementation ---

    void update(double deltaMs) override {
        if (StateNode* current = findState(m_currentState)) {
            if (current->onUpdate) {
                current->onUpdate(deltaMs);
            }
        }
    }

    const char* getCurrentStateName() const override {
        if (const StateNode* current = findState(m_currentState)) {
            return current->name;
        }
        return "Unknown";
    }

    int getCurrentStateId() const override {
        return static_cast<int>(m_currentState);
    }

    bool isInState(int stateId) const override {
        return static_cast<int>(m_currentState) == stateId;
    }

    bool dispatchEvent(int eventId) override {
        return postEvent(static_cast<TEvent>(eventId));
    }

    void reset() override {
        setState(m_initialState, true);
        m_transitionCount = 0;
    }

    size_t getTransitionCount() const override {
        return m_transitionCount;
    }

    const char* getName() const {
        return m_name;
    }

private:
    StateNode* findState(TState id) {
        for (auto& s : m_states) {
            if (s.id == id) return &s;
        }
        return nullptr;
    }

    const StateNode* findState(TState id) const {
        for (const auto& s : m_states) {
            if (s.id == id) return &s;
        }
        return nullptr;
    }

    const char* m_name;
    TState m_initialState;
    TState m_currentState;
    std::vector<StateNode> m_states;
    std::vector<Transition> m_transitions;
    TransitionObserver m_observer;
    size_t m_transitionCount = 0;
};

} // namespace evo

#endif // EVO_STATE_MACHINE_HPP
