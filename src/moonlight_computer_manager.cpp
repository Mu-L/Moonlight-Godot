#include "moonlight_computer_manager.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/marshalls.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

MoonlightComputerManager::MoonlightComputerManager() {
	moonlight_requester = memnew(MoonlightRequester);
	owns_config_manager = false;
}

MoonlightComputerManager::~MoonlightComputerManager() {
	if (moonlight_requester) {
		memdelete(moonlight_requester);
	}
	if (owns_config_manager && config_manager) {
		memdelete(config_manager);
	}
}

void MoonlightComputerManager::set_config_manager(Object *cm) {
	config_manager = Object::cast_to<MoonlightConfigManager>(cm);
	owns_config_manager = false; // 外部所有权
}

// 1. 配对逻辑

String MoonlightComputerManager::start_pair(String ip, int port) {
	// 如果缺少 config_manager，则在内部初始化一个默认的
	if (config_manager == nullptr) {
		config_manager = memnew(MoonlightConfigManager);
		owns_config_manager = true;
	}

	_reset_pairing();
	pair_ip = ip;
	pair_port = port;
	pair_https_port = cached_https_ports.get(ip, 47984);
	unique_id = _get_unique_id();
	current_uuid = _get_uuid();

	// 1. 生成一个 PIN 码（4 位随机数字）
	int pin_val = UtilityFunctions::randi() % 10000;
	pair_pin = String::num_int64(pin_val).pad_zeros(4);

	// 2. Generate salt and key
	// client.c: SHA256(salt + PIN) -> aes_key (AES-128 uses 16 bytes)
	pair_salt = _generate_random_bytes(16);
	pair_aes_key = _calculate_aes_key(pair_salt, pair_pin);

	// 3. 开始进程
	pair_state = PAIR_STAGE_0_PREFLIGHT;
	_step_pair();

	return pair_pin;
}

void MoonlightComputerManager::_step_pair() {
	if (pair_state == PAIR_IDLE || pair_state == PAIR_FINISHED || pair_state == PAIR_ERROR)
		return;

	is_requesting = true;
	String base_url = "http://" + pair_ip + ":" + String::num_int64(pair_port) + "/pair";
	String common_params = "uniqueid=" + unique_id + "&uuid=" + current_uuid + "&devicename=roth&updateState=1";

	Dictionary ssl_opts; // HTTP为空

	switch (pair_state) {
		case PAIR_STAGE_0_PREFLIGHT: {
			// Stage 0: Check server info to see if we are already paired and get server unique ID
			String url = "http://" + pair_ip + ":" + String::num_int64(pair_port) + "/serverinfo?uniqueid=" + unique_id + "&uuid=" + current_uuid;
			moonlight_requester->request(url, "GET", PackedByteArray(), Dictionary(), Dictionary(), callable_mp(this, &MoonlightComputerManager::_on_pair_request_completed).bind(0));
			break;
		}
		case PAIR_STAGE_1_GET_CERT: {
			//Stage 1: Send the salt and client certificate, obtain the server certificate. The server blocks here, waiting for the PIN code input.
			Dictionary keys = config_manager->get_client_keys();
			String client_cert_pem = keys["certificate"];

			// 修复：GameStream 协议要求发送 Hex 编码的 PEM 字符串（包含头尾），而不是 DER
			// 参考 client.c：它直接读取 PEM 文件并进行 Hex 编码发送
			PackedByteArray cert_bytes = client_cert_pem.to_utf8_buffer();

			String url = base_url + "?" + common_params + "&phrase=getservercert&salt=" + _bytes_to_hex(pair_salt) + "&clientcert=" + _bytes_to_hex(cert_bytes);
			moonlight_requester->request(url, "GET", PackedByteArray(), Dictionary(), ssl_opts, callable_mp(this, &MoonlightComputerManager::_on_pair_request_completed).bind(1));
			break;
		}
		case PAIR_STAGE_2_CLIENT_CHALLENGE: {
			// Stage 2: Send encrypted client challenge
			// Generate 16 bytes of random data
			client_secret_random = _generate_random_bytes(16);
			// Encrypt using AES key
			PackedByteArray challenge_enc = _encrypt_aes_ecb(client_secret_random, pair_aes_key);

			String url = base_url + "?" + common_params + "&clientchallenge=" + _bytes_to_hex(challenge_enc);
			moonlight_requester->request(url, "GET", PackedByteArray(), Dictionary(), ssl_opts, callable_mp(this, &MoonlightComputerManager::_on_pair_request_completed).bind(2));
			break;
		}
		case PAIR_STAGE_3_SERVER_RESPONSE: {
			// 第3阶段：响应服务器挑战
			// 负载：ServerChallenge   CertSig   ClientSecret
			// 注意：之前的逻辑错误地使用了server_secret，应该使用server_challenge。

			// 1. 获取客户端证书 DER 以提取签名
			Dictionary keys = config_manager->get_client_keys();
			String cert_clean = String(keys["certificate"]).replace("-----BEGIN CERTIFICATE-----", "").replace("-----END CERTIFICATE-----", "").replace("\n", "").replace("\r", "");
			PackedByteArray cert_der = Marshalls::get_singleton()->base64_to_raw(cert_clean);
			PackedByteArray cert_sig = _extract_signature_from_der(cert_der);

			// 2. 生成客户端密钥（16 字节）
			client_pairing_secret = _generate_random_bytes(16);

			// 3. 构建负载：服务器挑战   证书签名   客户端密钥
			PackedByteArray payload;
			payload.append_array(server_challenge); // 根据 nvhttp.cpp 从 server_secret 修正
			payload.append_array(cert_sig);
			payload.append_array(client_pairing_secret);

			// 4. 哈希 SHA256
			PackedByteArray hash = _sha256(payload);

			// 5. 加密哈希
			PackedByteArray hash_enc = _encrypt_aes_ecb(hash, pair_aes_key);

			String url = base_url + "?" + common_params + "&serverchallengeresp=" + _bytes_to_hex(hash_enc);
			moonlight_requester->request(url, "GET", PackedByteArray(), Dictionary(), ssl_opts, callable_mp(this, &MoonlightComputerManager::_on_pair_request_completed).bind(3));
			break;
		}
		case PAIR_STAGE_4_CLIENT_SECRET: {
			// 第4阶段：发送客户端密钥和签名
			// 有效载荷：客户端密钥   签名(客户端密钥)
			// 注意：根据 client.c 和 nvhttp.cpp，这一步不是加密的

			// 1. 使用客户端私钥对客户端密钥进行签名
			PackedByteArray signature = _sign_data(client_pairing_secret);

			// 2. 连接
			PackedByteArray payload = client_pairing_secret;
			payload.append_array(signature);

			// 3. 以 HEX（纯文本）发送
			String url = base_url + "?" + common_params + "&clientpairingsecret=" + _bytes_to_hex(payload);
			moonlight_requester->request(url, "GET", PackedByteArray(), Dictionary(), ssl_opts, callable_mp(this, &MoonlightComputerManager::_on_pair_request_completed).bind(4));
			break;
		}
		case PAIR_STAGE_5_HTTPS_CHALLENGE: {
			// Stage 5: Completed via HTTPS
			String https_url = "https://" + pair_ip + ":" + String::num_int64(pair_https_port) + "/pair";
			String url = https_url + "?" + common_params + "&phrase=pairchallenge";

			// Get standard SSL options (provides cert content and disables verify_peer)
			Dictionary stage5_ssl_opts = _get_ssl_options();

			// 移除：不再需要写入临时文件或根据IP区分验证策略，统一由 _get_ssl_options 处理
			// 证书固定已被全局禁用。

			moonlight_requester->request(url, "GET", PackedByteArray(), Dictionary(), stage5_ssl_opts, callable_mp(this, &MoonlightComputerManager::_on_pair_request_completed).bind(5));
			break;
		}
	}
}

void MoonlightComputerManager::_on_pair_request_completed(int code, PackedByteArray body, Dictionary headers, String error, int step) {
	is_requesting = false;
	bool failed = false;
	String fail_msg;

	if (code != 200) {
		// Code 0 or -1 (Curl error) or >= 400
		if (code <= 0 || code >= 400) {
			failed = true;
			fail_msg = "Network Error (" + String::num_int64(code) + "): " + error;
		}
	}

	String xml;
	bool is_paired = false;
	if (!failed) {
		xml = body.get_string_from_utf8();
		if (step == 0) {
			is_paired = _extract_xml_value(xml, "PairStatus") == "1";
		} else {
			is_paired = _extract_xml_value(xml, "paired") == "1";
		}

		// Check failed
		if (!is_paired && step != 1 && step != 0) {
			// Except for Stage 1 (waiting for PIN) and Stage 0 (preflight), paired=0 usually indicates failure
			failed = true;
			fail_msg = _extract_xml_value(xml, "status_message");
			if (fail_msg.is_empty())
				fail_msg = "Pairing failed at step " + String::num_int64(step);
		}
	}

	if (failed) {
		// Reference client.c: Cleanup sends unpair on failure
		// 发送 unpair 请求通知服务端清除状态 (Fire and forget)
		String uuid = _get_uuid();
		String url = "http://" + pair_ip + ":" + String::num_int64(pair_port) + "/unpair?uniqueid=" + unique_id + "&uuid=" + uuid;
		moonlight_requester->request(url, "GET", PackedByteArray(), Dictionary(), Dictionary(), Callable());

		pair_state = PAIR_ERROR;
		emit_signal("pair_completed", false, fail_msg);
		return;
	}

	switch (step) {
		case 0: { // Preflight check
			server_unique_id = _extract_xml_value(xml, "uniqueid");
			String https_port_str = _extract_xml_value(xml, "HttpsPort");
			if (!https_port_str.is_empty()) {
				pair_https_port = https_port_str.to_int();
				cached_https_ports[pair_ip] = pair_https_port;
			}

			// 即使服务端返回 Paired=1 也可能不可靠，因此仅依赖本地配置是否存在该 Server Unique ID
			bool known_and_paired = false;
			if (!server_unique_id.is_empty()) {
				Array hosts = config_manager->get_hosts();
				for (int i = 0; i < hosts.size(); i++) {
					Dictionary h = hosts[i];
					// 检查是否存在且 Unique ID 匹配
					if (h.get("server_unique_id", "") == server_unique_id) {
						known_and_paired = true;

						// 如果已配对但 IP 地址变更（例如 DHCP 分配了新 IP），则更新本地配置
						if (h.get("localaddress", "") != pair_ip) {
							Dictionary update_data;
							update_data["localaddress"] = pair_ip;
							config_manager->update_host(h["id"], update_data);
						}
						break;
					}
				}
			}

			if (known_and_paired) {
				pair_state = PAIR_FINISHED;
				emit_signal("pair_completed", true, "Already paired");
				return;
			}

			// Not paired or not found locally, proceed to pairing
			pair_state = PAIR_STAGE_1_GET_CERT;
			_step_pair();
			break;
		}
		case 1: { // Obtain Certificate
			if (!is_paired) {
				//Waiting for PIN input… If polling, the server may return paired=0, but it usually blocks. If we get paired=0, we may need to retry or fail. Currently, assume that if code=200 and plaincert is missing, it indicates failure or the user has canceled.
				String status_msg = _extract_xml_value(xml, "status_message");
				if (!status_msg.is_empty()) {
					pair_state = PAIR_ERROR;
					emit_signal("pair_completed", false, status_msg);
					return;
				}
				// Retry? Or just fail immediately. The README says it will block.
				// If the code reaches this point, the request has already been completed.
				// Assume failure if there is no certificate.
			}

			String plaincert = _extract_xml_value(xml, "plaincert");
			if (plaincert.is_empty()) {
				pair_state = PAIR_ERROR;
				emit_signal("pair_completed", false, "No server certificate received.");
				return;
			}

			// Store server certificate (from hexadecimal to PEM)
			// If we want to save it as PEM, it needs to be properly formatted, but for internal logic, we might just keep it in hexadecimal or raw format.
			// However, for later verify_signature (if we implement it), we need this key.
			// For now, just store it in raw format.
			server_cert_pem = _hex_to_bytes(plaincert).get_string_from_ascii(); // 假设十六进制解码为 PEM 字符串

			pair_state = PAIR_STAGE_2_CLIENT_CHALLENGE;
			_step_pair();
			break;
		}
		case 2: { // Server response received
			String resp_hex = _extract_xml_value(xml, "challengeresponse");
			if (resp_hex.is_empty()) {
				emit_signal("pair_completed", false, "Empty challenge response");
				return;
			}

			PackedByteArray resp_enc = _hex_to_bytes(resp_hex);
			PackedByteArray resp_dec = _decrypt_aes_ecb(resp_enc, pair_aes_key);

			// 解密数据结构：[SHA256 哈希（32 字节）]   [服务器挑战（16 字节）]
			// 总大小应为 48 字节（SHA1 为 36 字节，但我们使用 SHA256）

			if (resp_dec.size() < 48) {
				emit_signal("pair_completed", false, "Invalid server response size");
				return;
			}

			// 提取 ServerChallenge（最后 16 个字节）
			// client.c：memcpy(challenge_response, challenge_response_data + hash_length, 16);
			server_challenge = resp_dec.slice(32, 48);

			pair_state = PAIR_STAGE_3_SERVER_RESPONSE;
			_step_pair();
			break;
		}
		case 3: { // Pairing key obtained
			String pairing_secret_hex = _extract_xml_value(xml, "pairingsecret");

			// pairingsecret = ServerSecret   Signature(ServerSecret)
			// 如果需要，我们可以提取 ServerSecret，但该协议主要用它来进行中间人攻击验证。
			PackedByteArray pairing_secret = _hex_to_bytes(pairing_secret_hex);
			if (pairing_secret.size() >= 16) {
				server_secret = pairing_secret.slice(0, 16);
			}

			pair_state = PAIR_STAGE_4_CLIENT_SECRET;
			_step_pair();
			break;
		}
		case 4: { // The client key has been sent
			pair_state = PAIR_STAGE_5_HTTPS_CHALLENGE;
			_step_pair();
			break;
		}
		case 5: { // HTTPS 挑战
			// 成功！
			Dictionary host_data;
			host_data["hostname"] = pair_ip;
			host_data["localaddress"] = pair_ip;
			host_data["uuid"] = current_uuid;
			host_data["srvcert"] = server_cert_pem;
			host_data["https_port"] = pair_https_port;
			host_data["server_unique_id"] = server_unique_id;
			config_manager->add_host(host_data);

			pair_state = PAIR_FINISHED;
			emit_signal("pair_completed", true, "Pairing successful");
			break;
		}
	}
}

void MoonlightComputerManager::cancel_pair() {
	if (config_manager == nullptr) {
		config_manager = memnew(MoonlightConfigManager);
		owns_config_manager = true;
	}
	if (pair_state != PAIR_IDLE && pair_state != PAIR_FINISHED && pair_state != PAIR_ERROR) {
		// 参考 client.c: gs_unpair
		// 主动取消时发送 unpair 请求。注意：这里应生成一个新的随机 UUID，而不是使用当前的 current_uuid。
		String uuid = _get_uuid();
		String url = "http://" + pair_ip + ":" + String::num_int64(pair_port) + "/unpair?uniqueid=" + unique_id + "&uuid=" + uuid;
		moonlight_requester->request(url, "GET", PackedByteArray(), Dictionary(), Dictionary(), Callable());
	}
	_reset_pairing();
}

void MoonlightComputerManager::unpair(int host_id) {
	if (config_manager == nullptr) {
		config_manager = memnew(MoonlightConfigManager);
		owns_config_manager = true;
	}
	if (config_manager) {
		config_manager->remove_host(host_id);
	}
}

void MoonlightComputerManager::_reset_pairing() {
	pair_state = PAIR_IDLE;
	is_requesting = false;
	server_unique_id = "";
	server_cert_pem = "";
	server_secret.clear();
	server_challenge.clear();
	client_secret_random.clear();
	client_pairing_secret.clear();
}

// 2. 测试连接

void MoonlightComputerManager::connect_to_computer(String ip, int port, Callable callback) {
	// 如果缺少 config_manager，则在内部初始化一个默认的
	if (config_manager == nullptr) {
		config_manager = memnew(MoonlightConfigManager);
		owns_config_manager = true;
	}
	String url = "http://" + ip + ":" + String::num_int64(port) + "/serverinfo?uniqueid=" + _get_unique_id() + "&uuid=" + _get_uuid();
	moonlight_requester->request(url, "GET", PackedByteArray(), Dictionary(), Dictionary(),
			callable_mp(this, &MoonlightComputerManager::_on_server_info_completed).bind(Variant(callback), Variant(ip)));
}

void MoonlightComputerManager::_on_server_info_completed(int code, PackedByteArray body, Dictionary headers, String error, Callable callback, String ip) {
	Dictionary result;
	result["status"] = (code == 200) ? "online" : "offline";
	if (code == 200) {
		String xml = body.get_string_from_utf8();
		result["hostname"] = _extract_xml_value(xml, "hostname");
		result["uniqueid"] = _extract_xml_value(xml, "uniqueid");
		result["paired"] = _extract_xml_value(xml, "PairStatus") == "1";
		result["ip"] = ip;

		String port_str = _extract_xml_value(xml, "HttpsPort");
		int port = port_str.is_empty() ? 47984 : port_str.to_int();
		result["https_port"] = port;
		cached_https_ports[ip] = port;
	}
	if (callback.is_valid())
		callback.call(result);
}

// 3. 应用列表及 Cover

void MoonlightComputerManager::get_app_list(int host_id, Callable callback) {
	// 如果缺少 config_manager，则在内部初始化一个默认的
	if (config_manager == nullptr) {
		config_manager = memnew(MoonlightConfigManager);
		owns_config_manager = true;
	}
	Array hosts = config_manager->get_hosts();
	String ip;
	int port = 47984;
	for (int i = 0; i < hosts.size(); i++) {
		Dictionary host = hosts[i];
		if ((int64_t)host["id"] == host_id) {
			ip = host.get("localaddress", "");
			port = host.get("https_port", 47984);
		}
	}
	if (ip.is_empty())
		return;

	String url = "https://" + ip + ":" + String::num_int64(port) + "/applist?uniqueid=" + unique_id + "&uuid=" + _get_uuid();
	moonlight_requester->request(url, "GET", PackedByteArray(), Dictionary(), _get_ssl_options(),
			callable_mp(this, &MoonlightComputerManager::_on_app_list_completed).bind(Variant(host_id), Variant(callback)));
}

void MoonlightComputerManager::_on_app_list_completed(int code, PackedByteArray body, Dictionary headers, String error, int host_id, Callable callback) {
	if (code == 200) {
		// 获取现有应用列表以进行去重
		Array existing_apps = config_manager->get_apps(host_id);
		Array existing_ids;
		for (int i = 0; i < existing_apps.size(); i++) {
			Dictionary app = existing_apps[i];
			existing_ids.append(app.get("id", 0));
		}

		String xml = body.get_string_from_utf8();
		int pos = 0;
		while (true) {
			int start = xml.find("<App>", pos);
			if (start == -1)
				break;
			int end = xml.find("</App>", start);
			if (end == -1)
				break;

			String app_xml = xml.substr(start, end - start + 6);
			Dictionary app_data;
			app_data["name"] = _extract_xml_value(app_xml, "AppTitle");
			int app_id = _extract_xml_value(app_xml, "ID").to_int();
			app_data["id"] = app_id;

			// 仅添加不存在的 ID
			if (!existing_ids.has(app_id)) {
				config_manager->add_app(host_id, app_data);
				existing_ids.append(app_id);
			}

			pos = end + 6;
		}
	}
	if (callback.is_valid())
		callback.call(code == 200);
}

void MoonlightComputerManager::get_app_cover(int host_id, int app_id, Callable callback) {
	// 如果缺少 config_manager，则在内部初始化一个默认的
	if (config_manager == nullptr) {
		config_manager = memnew(MoonlightConfigManager);
		owns_config_manager = true;
	}

	Array hosts = config_manager->get_hosts();
	String ip;
	int port = 47984;
	for (int i = 0; i < hosts.size(); i++) {
		Dictionary host = hosts[i];
		if ((int64_t)host["id"] == host_id) {
			ip = host.get("localaddress", "");
			port = host.get("https_port", 47984);
		}
	}
	if (ip.is_empty())
		return;

	String url = "https://" + ip + ":" + String::num_int64(port) + "/appasset?uniqueid=" + unique_id + "&uuid=" + _get_uuid() + "&appid=" + String::num_int64(app_id);
	moonlight_requester->request(url, "GET", PackedByteArray(), Dictionary(), _get_ssl_options(),
			callable_mp(this, &MoonlightComputerManager::_on_app_cover_completed).bind(Variant(callback)));
}

void MoonlightComputerManager::_on_app_cover_completed(int code, PackedByteArray body, Dictionary headers, String error, Callable callback) {
	if (code == 200 && callback.is_valid()) {
		Ref<Image> img;
		img.instantiate();
		if (img->load_png_from_buffer(body) == OK || img->load_jpg_from_buffer(body) == OK) {
			Ref<ImageTexture> tex = ImageTexture::create_from_image(img);
			callback.call(tex);
			return;
		}
	}
	if (callback.is_valid())
		callback.call(Variant());
}

// 4. 流连接管理

void MoonlightComputerManager::establish_stream(int host_id, int app_id, Dictionary options, Callable callback) {
	// 如果缺少 config_manager，则在内部初始化一个默认的
	if (config_manager == nullptr) {
		config_manager = memnew(MoonlightConfigManager);
		owns_config_manager = true;
	}
	Array hosts = config_manager->get_hosts();
	String ip;
	int port = 47984;
	for (int i = 0; i < hosts.size(); i++) {
		Dictionary host = hosts[i];
		if ((int64_t)host["id"] == host_id) {
			ip = host.get("localaddress", "");
			port = host.get("https_port", 47984);
			break;
		}
	}

	if (ip.is_empty()) {
		if (callback.is_valid()) {
			Dictionary err;
			err["status"] = "error";
			err["message"] = "Host not found";
			callback.call(err);
		}
		return;
	}

	// 1. 准备参数和密钥
	Dictionary ctx;
	ctx["ip"] = ip;
	ctx["port"] = port;
	ctx["app_id"] = app_id;
	ctx["options"] = options;
	ctx["callback"] = callback;

	// 生成 rikey (128-bit AES key, Hex string)
	PackedByteArray rikey_bytes = _generate_random_bytes(16);
	ctx["rikey"] = _bytes_to_hex(rikey_bytes);

	// 生成 rikeyid (Random integer)
	PackedByteArray rikeyid_bytes = _generate_random_bytes(4);
	int64_t rikeyid = rikeyid_bytes.decode_u32(0);
	ctx["rikeyid"] = rikeyid;

	// 2. 首先检查服务器状态 (/serverinfo) 以决定策略
	String url = "https://" + ip + ":" + String::num_int64(port) + "/serverinfo?uniqueid=" + unique_id + "&uuid=" + _get_uuid();
	moonlight_requester->request(url, "GET", PackedByteArray(), Dictionary(), _get_ssl_options(),
			callable_mp(this, &MoonlightComputerManager::_on_launch_serverinfo_completed).bind(ctx));
}

void MoonlightComputerManager::_on_launch_serverinfo_completed(int code, PackedByteArray body, Dictionary headers, String error, Dictionary ctx) {
	if (code != 200) {
		Callable cb = ctx["callback"];
		if (cb.is_valid()) {
			Dictionary res;
			res["status"] = "error";
			res["message"] = "Server check failed: " + error;
			cb.call(res);
		}
		return;
	}

	String xml = body.get_string_from_utf8();
	String current_game_str = _extract_xml_value(xml, "currentgame");
	int current_game = current_game_str.to_int();

	// 提取 ServerCodecModeSupport 并存入 ctx
	String scms_str = _extract_xml_value(xml, "ServerCodecModeSupport");
	if (!scms_str.is_empty()) {
		// 关键：将服务端支持的 codec 模式放入 options，供 StreamCore 内部的 Limelight 配置使用
		if (!ctx.has("options"))
			ctx["options"] = Dictionary();
		Dictionary opts = ctx["options"];
		opts["server_codec_mode_support"] = scms_str.to_int();

		// 同时存入 GfeVersion 和 AppVersion，因为 StreamCore 启动连接时需要这些信息
		String app_version = _extract_xml_value(xml, "appversion");
		String gfe_version = _extract_xml_value(xml, "GfeVersion");
		opts["app_version"] = app_version;
		opts["gfe_version"] = gfe_version;

		ctx["options"] = opts;
	}

	// 提取 appversion (修复断言失败的关键)
	String app_version = _extract_xml_value(xml, "appversion");
	if (!app_version.is_empty()) {
		ctx["app_version"] = app_version;
	}

	// 提取 GfeVersion
	String gfe_version = _extract_xml_value(xml, "GfeVersion");
	if (!gfe_version.is_empty()) {
		ctx["gfe_version"] = gfe_version;
	}

	// 逻辑：如果服务端已有游戏运行 (currentgame != 0)，则使用 /resume，否则使用 /launch
	String command = (current_game != 0) ? "resume" : "launch";

	// 进入实际请求阶段
	_perform_launch_request(ctx, command);
}

void MoonlightComputerManager::_perform_launch_request(Dictionary ctx, String command) {
	String ip = ctx["ip"];
	int port = ctx["port"];
	int app_id = ctx["app_id"];
	Dictionary options = ctx["options"];

	// rikey 和 rikeyid 是在 establish_stream 中生成的，但在 perform_launch 时需要填入 URL
	// 同时也需要确保它们存在于 options 中，以便最终传递给 StreamCore (因为 StreamCore 也是从 options 读取的)
	String rikey = ctx["rikey"];
	int64_t rikeyid = ctx["rikeyid"];

	options["rikey"] = rikey;
	options["rikeyid"] = rikeyid;
	options["ip"] = ip; // 确保 IP 也传递

	// 构造基本 URL (Required params)
	String url = "https://" + ip + ":" + String::num_int64(port) + "/" + command + "?uniqueid=" + unique_id + "&uuid=" + _get_uuid();
	url += "&appid=" + String::num_int64(app_id);
	url += "&rikey=" + rikey;
	url += "&rikeyid=" + String::num_int64(rikeyid);

	// 如果上游通过 Limelight 提供了额外的查询参数片段，则附加它（例如来自 LiGetLaunchUrlQueryParameters）
	if (options.has("limelight_query_parameters")) {
		String frag = options["limelight_query_parameters"];
		if (!frag.is_empty()) {
			// Frag 预期包含以 '&' 开头或不包含，直接附加即可
			url += frag;
		}
	}

	// 兼容性：接受 camelCase 的 surroundAudioInfo 键并规范为下划线版本
	if (options.has("surroundAudioInfo") && !options.has("surround_audio_info")) {
		options["surround_audio_info"] = options["surroundAudioInfo"];
	}

	// 可选参数 - 无默认值 (仅当 options 包含时添加)
	if (options.has("width") && options.has("height") && options.has("fps")) {
		String mode = String::num_int64(options["width"]) + "x" + String::num_int64(options["height"]) + "x" + String::num_int64(options["fps"]);
		url += "&mode=" + mode;
	}

	if (options.has("sops")) {
		url += "&sops=" + String::num_int64(options["sops"]);
	}
	if (options.has("surround_audio_info")) {
		url += "&surroundAudioInfo=" + String::num_int64(options["surround_audio_info"]);
	}
	if (options.has("surround_params")) {
		url += "&surroundParams=" + String(options["surround_params"]);
	}
	if (options.has("remote_controllers_bitmap")) {
		url += "&remoteControllersBitmap=" + String::num_int64(options["remote_controllers_bitmap"]);
	}
	if (options.has("gcmap")) {
		url += "&gcmap=" + String::num_int64(options["gcmap"]);
	}
	if (options.has("additional_states")) {
		url += "&additionalStates=" + String::num_int64(options["additional_states"]);
	}
	if (options.has("corever")) {
		url += "&corever=" + String::num_int64(options["corever"]);
	}
	if (options.has("continuous_audio")) {
		url += "&continuousAudio=" + String::num_int64(options["continuous_audio"]);
	}

	int hdr_mode = options.get("hdr_mode", 0);
	if (options.has("hdr_mode")) {
		url += "&hdrMode=" + String::num_int64(hdr_mode);
	}

	// 可选参数 - 有默认值 (总是添加)
	int local_audio = options.get("local_audio_play_mode", 1);
	url += "&localAudioPlayMode=" + String::num_int64(local_audio);

	if (hdr_mode == 1) {
		// HDR 启用时，相关参数变为使用 README 中的默认值
		url += "&clientHdrCapVersion=" + String::num_int64(options.get("client_hdr_cap_version", 0));
		url += "&clientHdrCapSupportedFlagsInUint32=" + String::num_int64(options.get("client_hdr_cap_supported_flags", 0));
		url += "&clientHdrCapMetaDataId=" + String(options.get("client_hdr_cap_meta_data_id", "NV_STATIC_METADATA_TYPE_1"));
		url += "&clientHdrCapDisplayData=" + String(options.get("client_hdr_cap_display_data", "0x0x0x0x0x0x0x0x0x0x0"));
	}

	// 遍历 options 添加未被显式处理的自定义参数
	Array keys = options.keys();
	for (int i = 0; i < keys.size(); i++) {
		String key = keys[i];
		// 跳过已知的、已手动处理的键
		if (key == "width" || key == "height" || key == "fps" ||
				key == "sops" || key == "surround_audio_info" || key == "surround_params" ||
				key == "remote_controllers_bitmap" || key == "gcmap" || key == "additional_states" ||
				key == "corever" || key == "continuous_audio" ||
				key == "hdr_mode" || key == "local_audio_play_mode" ||
				key == "client_hdr_cap_version" || key == "client_hdr_cap_supported_flags" ||
				key == "client_hdr_cap_meta_data_id" || key == "client_hdr_cap_display_data") {
			continue;
		}

		// Skip Limelight-provided fragment which we already appended
		if (key == "limelight_query_parameters")
			continue;
		// 原样追加其他参数
		url += "&" + key + "=" + String(options[key]);
	}

	// 发送请求
	moonlight_requester->request(url, "GET", PackedByteArray(), Dictionary(), _get_ssl_options(),
			callable_mp(this, &MoonlightComputerManager::_on_launch_request_completed).bind(ctx));
}

void MoonlightComputerManager::_on_launch_request_completed(int code, PackedByteArray body, Dictionary headers, String error, Dictionary ctx) {
	Callable cb = ctx["callback"];
	if (!cb.is_valid())
		return;

	Dictionary response = ctx.duplicate();
	response.erase("callback"); // 清理回调引用

	if (code == 200) {
		String xml = body.get_string_from_utf8();
		String session_url = _extract_xml_value(xml, "sessionUrl0");

		if (!session_url.is_empty()) {
			response["status"] = "success";
			response["session_url"] = session_url;
			// 成功：返回包含所有请求参数(rikey等)和session_url的字典

			// 平铺 options 到顶层，以便 StreamCore 可以直接读取 width/height 等
			Dictionary opts = ctx.get("options", Dictionary());
			opts["session_url"] = session_url; // 确保 session_url 进入 options

			Array keys = opts.keys();
			for (int i = 0; i < keys.size(); i++) {
				response[keys[i]] = opts[keys[i]];
			}
		} else {
			response["status"] = "error";
			response["message"] = "Session URL not found in response. Game may be stuck.";
			response["xml_debug"] = xml;
		}
	} else {
		response["status"] = "error";
		response["message"] = "Launch/Resume failed (" + String::num_int64(code) + "): " + error;
	}

	cb.call(response);
}

void MoonlightComputerManager::stop_stream(int host_id, Callable callback) {
	// 如果缺少 config_manager，则在内部初始化一个默认的
	if (config_manager == nullptr) {
		config_manager = memnew(MoonlightConfigManager);
		owns_config_manager = true;
	}
	Array hosts = config_manager->get_hosts();
	String ip;
	int port = 47984;
	for (int i = 0; i < hosts.size(); i++) {
		Dictionary host = hosts[i];
		if ((int64_t)host["id"] == host_id) {
			ip = host.get("localaddress", "");
			port = host.get("https_port", 47984);
		}
	}

	String url = "https://" + ip + ":" + String::num_int64(port) + "/cancel?uniqueid=" + unique_id + "&uuid=" + _get_uuid();
	moonlight_requester->request(url, "GET", PackedByteArray(), Dictionary(), _get_ssl_options(),
			callable_mp(this, &MoonlightComputerManager::_on_simple_request_completed).bind(Variant(callback)));
}

void MoonlightComputerManager::_on_simple_request_completed(int code, PackedByteArray body, Dictionary headers, String error, Callable callback) {
	if (callback.is_valid())
		callback.call(code == 200 ? body.get_string_from_utf8() : "");
}

// 工具函数

PackedByteArray MoonlightComputerManager::_generate_random_bytes(int size) {
	Ref<Crypto> c;
	c.instantiate();
	return c->generate_random_bytes(size);
}

PackedByteArray MoonlightComputerManager::_calculate_aes_key(const PackedByteArray &salt, const String &pin) {
	PackedByteArray combined = salt;
	// 将 PIN 转换为 ASCII 字节（例如 "1234" -> 0x31 0x32 0x33 0x34）
	combined.append_array(pin.to_ascii_buffer());
	// Return the first 16 bytes of SHA256
	return _sha256(combined).slice(0, 16);
}

PackedByteArray MoonlightComputerManager::_encrypt_aes_ecb(const PackedByteArray &data, const PackedByteArray &key) {
	Ref<AESContext> ctx;
	ctx.instantiate();
	ctx->start(AESContext::MODE_ECB_ENCRYPT, key);
	PackedByteArray out = ctx->update(data);
	ctx->finish();
	return out;
}

PackedByteArray MoonlightComputerManager::_decrypt_aes_ecb(const PackedByteArray &data, const PackedByteArray &key) {
	Ref<AESContext> ctx;
	ctx.instantiate();
	ctx->start(AESContext::MODE_ECB_DECRYPT, key);
	PackedByteArray out = ctx->update(data);
	ctx->finish();
	return out;
}

PackedByteArray MoonlightComputerManager::_sha256(const PackedByteArray &data) {
	Ref<HashingContext> ctx;
	ctx.instantiate();
	ctx->start(HashingContext::HASH_SHA256);
	ctx->update(data);
	return ctx->finish();
}

PackedByteArray MoonlightComputerManager::_sign_data(const PackedByteArray &data) {
	Dictionary keys = config_manager->get_client_keys();
	Ref<Crypto> c;
	c.instantiate();
	Ref<CryptoKey> key;
	key.instantiate();
	if (key->load_from_string(keys["key"]) != OK)
		return PackedByteArray();

	// Godot Crypto::sign 当使用 HASH_SHA256 时，需要传入 32 字节的哈希摘要(Digest)
	PackedByteArray hash = _sha256(data);
	return c->sign(HashingContext::HASH_SHA256, hash, key);
}

PackedByteArray MoonlightComputerManager::_extract_signature_from_der(const PackedByteArray &der) {
	// A minimal ASN.1 parser used to find the last BIT STRING in a sequence
	// X.509 structure: SEQUENCE { ... }
	// Inside: TBSCertificate, AlgorithmIdentifier, BIT STRING (signature)

	int len = der.size();
	const uint8_t *data = der.ptr();
	int pos = 0;

	// 1. Check external sequence
	if (pos >= len || data[pos++] != 0x30)
		return PackedByteArray(); // Not a sequence

	// Skip length byte
	if (pos >= len)
		return PackedByteArray();
	if (data[pos] & 0x80) {
		int len_bytes = data[pos] & 0x7F;
		pos += 1 + len_bytes;
	} else {
		pos++;
	}

	//We expect 3 child elements. The signature is the third one (bit string, tag 0x03). We iterate through the child elements.
	int child_count = 0;
	while (pos < len) {
		uint8_t tag = data[pos];

		// If we find the third element and it is a BIT STRING (0x03)
		if (child_count == 2) {
			if (tag == 0x03) {
				pos++; // Skip tags
				// Parse length
				int val_len = 0;
				if (data[pos] & 0x80) {
					int len_bytes = data[pos] & 0x7F;
					pos++;
					for (int i = 0; i < len_bytes; i++) {
						val_len = (val_len << 8) | data[pos++];
					}
				} else {
					val_len = data[pos++];
				}

				// A BIT STRING has 1 byte at the beginning to count the unused bits.
				if (val_len > 1) {
					// Return bytes, skipping bytes with unused bits
					return der.slice(pos + 1, pos + val_len);
				}
				return PackedByteArray();
			}
			return PackedByteArray(); // Isn't the third element a bit string?
		}

		// Skip current element
		pos++; // Skip tags
		// Parse the length to skip
		int val_len = 0;
		if (pos < len) {
			if (data[pos] & 0x80) {
				int len_bytes = data[pos] & 0x7F;
				pos++;
				for (int i = 0; i < len_bytes; i++) {
					if (pos >= len)
						return PackedByteArray();
					val_len = (val_len << 8) | data[pos++];
				}
			} else {
				val_len = data[pos++];
			}
		}
		pos += val_len;
		child_count++;
	}

	return PackedByteArray();
}

String MoonlightComputerManager::_bytes_to_hex(const PackedByteArray &bytes) {
	return bytes.hex_encode().to_lower();
}

PackedByteArray MoonlightComputerManager::_hex_to_bytes(const String &hex) {
	return hex.hex_decode();
}

String MoonlightComputerManager::_extract_xml_value(const String &xml, const String &tag) {
	String start_tag = "<" + tag + ">";
	String end_tag = "</" + tag + ">";
	int start = xml.find(start_tag);
	if (start == -1)
		return "";
	start += start_tag.length();
	int end = xml.find(end_tag, start);
	if (end == -1)
		return "";
	return xml.substr(start, end - start);
}

String MoonlightComputerManager::_get_unique_id() {
	if (config_manager->get_custom_data(MoonlightConfigManager::TARGET_GLOBAL, 0, 0, "uniqueid", "").stringify().is_empty()) {
		String uid = _bytes_to_hex(_generate_random_bytes(8)).to_upper();
		config_manager->set_custom_data(MoonlightConfigManager::TARGET_GLOBAL, 0, 0, "uniqueid", uid);
	}
	return config_manager->get_custom_data(MoonlightConfigManager::TARGET_GLOBAL, 0, 0, "uniqueid", "").stringify();
}

String MoonlightComputerManager::_get_uuid() {
	return _bytes_to_hex(_generate_random_bytes(16)); // A standard UUID is 16 bytes
}

Dictionary MoonlightComputerManager::_get_ssl_options() {
	// 获取证书内容字符串 (非路径)
	Dictionary keys = config_manager->get_client_keys();
	Dictionary opts;

	// 映射到 MoonlightRequester 期望的键名
	opts["client_cert"] = keys["certificate"];
	opts["client_key"] = keys["key"];

	// 策略：始终禁用 Peer 验证（因服务端证书 CN 为非标准 "Sunshine Gamestream Host" 且不匹配 IP）
	opts["verify_peer"] = false;

	return opts;
}

void MoonlightComputerManager::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_config_manager", "cm"), &MoonlightComputerManager::set_config_manager);

	ClassDB::bind_method(D_METHOD("start_pair", "ip", "port"), &MoonlightComputerManager::start_pair, DEFVAL(47989));
	ClassDB::bind_method(D_METHOD("cancel_pair"), &MoonlightComputerManager::cancel_pair);
	ClassDB::bind_method(D_METHOD("unpair", "host_id"), &MoonlightComputerManager::unpair);

	ClassDB::bind_method(D_METHOD("connect_to_computer", "ip", "port", "callback"), &MoonlightComputerManager::connect_to_computer, DEFVAL(47989), DEFVAL(Callable()));
	ClassDB::bind_method(D_METHOD("get_app_list", "host_id", "callback"), &MoonlightComputerManager::get_app_list, DEFVAL(Callable()));
	ClassDB::bind_method(D_METHOD("get_app_cover", "host_id", "app_id", "callback"), &MoonlightComputerManager::get_app_cover);
	ClassDB::bind_method(D_METHOD("establish_stream", "host_id", "app_id", "options", "callback"), &MoonlightComputerManager::establish_stream);
	ClassDB::bind_method(D_METHOD("stop_stream", "host_id", "callback"), &MoonlightComputerManager::stop_stream);

	ClassDB::bind_method(D_METHOD("_on_pair_request_completed"), &MoonlightComputerManager::_on_pair_request_completed);
	ClassDB::bind_method(D_METHOD("_on_server_info_completed"), &MoonlightComputerManager::_on_server_info_completed);
	ClassDB::bind_method(D_METHOD("_on_app_list_completed"), &MoonlightComputerManager::_on_app_list_completed);
	ClassDB::bind_method(D_METHOD("_on_app_cover_completed"), &MoonlightComputerManager::_on_app_cover_completed);
	ClassDB::bind_method(D_METHOD("_on_simple_request_completed"), &MoonlightComputerManager::_on_simple_request_completed);

	ClassDB::bind_method(D_METHOD("_on_launch_serverinfo_completed"), &MoonlightComputerManager::_on_launch_serverinfo_completed);
	ClassDB::bind_method(D_METHOD("_on_launch_request_completed"), &MoonlightComputerManager::_on_launch_request_completed);

	ADD_SIGNAL(MethodInfo("pair_completed", PropertyInfo(Variant::BOOL, "success"), PropertyInfo(Variant::STRING, "message")));
}
