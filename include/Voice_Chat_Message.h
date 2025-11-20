#ifndef VOICE_CHAT_MESSAGE_H
#define VOICE_CHAT_MESSAGE_H
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "miniaudio.h"

class voice_chat_message {
public:
	static constexpr std::size_t max_encoded_length = 4000;
	static constexpr std::size_t token_length = 16;
	static constexpr std::size_t header_length = 21;
	uint8_t token_len;//1
	std::string token;//16
	uint8_t room_id;//1
	uint8_t sender_id;//1
	uint16_t len;//2
	void set_header(std::string token_, int encoded_bytes_, uint8_t room_id_, uint8_t sender_id_) {
		token_len = token_length;
		token = token_;
		encoded_bytes = encoded_bytes_;
		len = static_cast<uint16_t>(encoded_bytes);
		std::memcpy(data, &token_len, sizeof(token_len));
		std::memcpy(data + sizeof(token_len), token.c_str(), token_length);
		std::memcpy(data + sizeof(token_len) + token_length, &room_id_, sizeof(room_id_));
		std::memcpy(data + sizeof(token_len) + token_length + sizeof(room_id_), &sender_id_, sizeof(sender_id_));
		std::memcpy(data + sizeof(token_len) + token_length + sizeof(room_id_) + sizeof(sender_id), &len, sizeof(len));
	}
	void decode_header(uint8_t* array) {
		token_len = (array)[0];
		token.assign(reinterpret_cast<char*>(array + 1), token_length);
		room_id = (array)[token_length + 1];
		sender_id = array[token_length + 2];
		len = static_cast<uint16_t>((array)[token_length + 3]) |
			static_cast<uint16_t>((array)[token_length + 4]) << 8;
	}
	uint8_t* get_data() {
		return data;
	}
private:
	int encoded_bytes;
	uint8_t data[header_length];
};
#endif