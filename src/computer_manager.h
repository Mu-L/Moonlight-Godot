#pragma once

#include "config_manager.h"
#include "moonlight_godot.h"
#include "requester.h"
#include <godot_cpp/classes/aes_context.hpp>
#include <godot_cpp/classes/crypto.hpp>
#include <godot_cpp/classes/crypto_key.hpp>
#include <godot_cpp/classes/hashing_context.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/x509_certificate.hpp>

namespace godot {

class ComputerManager : public MoonlightGodot {
	GDCLASS(ComputerManager, MoonlightGodot)

private:
	ConfigManager *config_manager = nullptr;
	Requester *requester = nullptr;

	// Pairing State
	enum PairState {
		PAIR_IDLE,
		PAIR_GET_CERT, // Phase 1: Send Salt, Get Server Cert
		PAIR_CLIENT_CHALLENGE, // Phase 2: Send Client Challenge
		PAIR_SERVER_CHALLENGE_RESP, // Phase 3: Send Server Challenge Response
		PAIR_CLIENT_PAIRING_SECRET, // Phase 4: Send Client Pairing Secret
		PAIR_HTTPS_PAIR_CHALLENGE, // Phase 5: Finalize via HTTPS
		PAIR_FINISHED,
		PAIR_ERROR
	};

	PairState pair_state = PAIR_IDLE;
	String pair_ip;
	int pair_port;
	int pair_https_port = 47984;
	String pair_pin;
	PackedByteArray pair_salt;
	PackedByteArray pair_aes_key;
	String unique_id;

	// Pairing temporary data
	String server_cert_pem;
	bool is_requesting = false;
	String last_error;

	Dictionary cached_https_ports;

	// Helpers
	void _reset_pairing();
	PackedByteArray _generate_random_bytes(int size);
	String _bytes_to_hex(const PackedByteArray &bytes);
	PackedByteArray _hex_to_bytes(const String &hex);
	PackedByteArray _calculate_aes_key(const PackedByteArray &salt, const String &pin);
	PackedByteArray _encrypt_aes_ecb(const PackedByteArray &data, const PackedByteArray &key);
	PackedByteArray _decrypt_aes_ecb(const PackedByteArray &data, const PackedByteArray &key);
	PackedByteArray _sign_data(const PackedByteArray &data);
	PackedByteArray _sha256(const PackedByteArray &data);

	String _get_unique_id();
	String _get_uuid();
	Dictionary _get_ssl_options();

	// Callbacks
	void _on_pair_request_completed(int code, PackedByteArray body, Dictionary headers, String error, int step);
	void _on_server_info_completed(int code, PackedByteArray body, Dictionary headers, String error, Callable callback, String ip);
	void _on_app_list_completed(int code, PackedByteArray body, Dictionary headers, String error, int host_id, Callable callback);
	void _on_app_cover_completed(int code, PackedByteArray body, Dictionary headers, String error, Callable callback);
	void _on_simple_request_completed(int code, PackedByteArray body, Dictionary headers, String error, Callable callback);

	String _extract_xml_value(const String &xml, const String &tag);

protected:
	static void _bind_methods();

public:
	ComputerManager();
	~ComputerManager();

	void set_config_manager(Object *cm);

	// 1. Pairing
	String start_pair(String ip, int port = 47989);
	String complete_pair(); // Returns: "working", "paired", "failed", "pin_needed"
	void cancel_pair();
	void unpair(int host_id);

	// 2. Connection
	void connect_to_computer(String ip, int port = 47989, Callable callback = Callable());

	// 3. Apps
	void get_app_list(int host_id, Callable callback = Callable());
	void get_app_cover(int host_id, int app_id, Callable callback);

	// 4. Stream
	void establish_stream(int host_id, int app_id, Dictionary options, Callable callback);
	void stop_stream(int host_id, Callable callback);
};

} //namespace godot
