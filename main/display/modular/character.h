#ifndef MODULAR_CHARACTER_H
#define MODULAR_CHARACTER_H

#include <lvgl.h>

// States every character pack must be able to show. A pack may map several
// states to the same art; the UI engine only ever asks for a state.
enum class CharacterState {
    kIdle,
    kListening,
    kThinking,
    kSpeaking,
    kJoy,
    kSurprise,
    kThoughtful,
    kSad,
    kSleep,
    kGreeting,
    kOffline,
    kDone,
};

// Maps protocol emotion names ("happy", "sad", ...) to a character state.
CharacterState CharacterStateFromEmotion(const char* emotion);

// A character draws itself inside a parent object. All methods must be called
// with the LVGL lock held.
class Character {
public:
    virtual ~Character() = default;
    virtual void Attach(lv_obj_t* parent) = 0;
    virtual void SetState(CharacterState state) = 0;
};

// Built-in code-drawn character (no image assets needed).
Character* CreateProceduralCharacter();

#endif  // MODULAR_CHARACTER_H
