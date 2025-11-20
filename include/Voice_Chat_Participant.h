#include <string>
#include <vector>
#include <memory>
#include "Voice_Chat_Message.h"

#ifndef VOICE_CHAT_PARTICIPANT_H
#define VOICE_CHAT_PARTICIPANT_H
class voice_chat_participant {
public:
	std::uint8_t id;
	virtual ~voice_chat_participant() {}
	virtual void deliver(const voice_chat_message& msg) {};
};
#endif