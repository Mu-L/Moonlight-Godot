#include "config_manager.h"
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

ConfigManager::ConfigManager() {
	config.instantiate();
	config_path = "user://addons/moonlight-godot/config.ini";
	load_config();
}

ConfigManager::~ConfigManager() {
}

void ConfigManager::load_config() {
	String dir = config_path.get_base_dir();
	Ref<DirAccess> da = DirAccess::open("user://");
	if (!da->dir_exists(dir)) {
		da->make_dir_recursive(dir);
	}

	if (config->load(config_path) != OK) {
		// 新配置，初始化结构
		save_config();
	}
	_check_and_create_certs();
}

void ConfigManager::save_config() {
	config->save(config_path);
}

String ConfigManager::_format_pem_for_qt(String pem) {
	// Qt ini 格式示例中使用了 @ByteArray 和显式 \n
	String out = pem.replace("\n", "\\n");
	return "@ByteArray(" + out + ")";
}

void ConfigManager::_check_and_create_certs() {
	if (!config->has_section_key("General", "certificate") || !config->has_section_key("General", "key")) {
		// 修复：Crypto 需要实例化，而非作为单例获取
		Ref<Crypto> crypto;
		crypto.instantiate();

		Ref<CryptoKey> key = crypto->generate_rsa(2048);
		Ref<X509Certificate> cert = crypto->generate_self_signed_certificate(key, "CN=NVIDIA GameStream Client");

		String key_pem = key->save_to_string();
		String cert_pem = cert->save_to_string();

		config->set_value("General", "certificate", _format_pem_for_qt(cert_pem));
		config->set_value("General", "key", _format_pem_for_qt(key_pem));
		save_config();
	}
}

Dictionary ConfigManager::get_client_keys() {
	Dictionary d;
	String cert = config->get_value("General", "certificate");
	String key = config->get_value("General", "key");

	// 解析 Qt 格式：去除 @ByteArray() 包装并还原换行符
	if (cert.begins_with("@ByteArray(") && cert.ends_with(")")) {
		cert = cert.substr(11, cert.length() - 12).replace("\\n", "\n");
	}
	if (key.begins_with("@ByteArray(") && key.ends_with(")")) {
		key = key.substr(11, key.length() - 12).replace("\\n", "\n");
	}

	d["certificate"] = cert;
	d["key"] = key;
	return d;
}

String ConfigManager::_get_host_prefix(int index) {
	return String::num_int64(index) + "\\";
}

String ConfigManager::_get_app_prefix(int host_index, int app_index) {
	return _get_host_prefix(host_index) + "apps\\" + String::num_int64(app_index) + "\\";
}

Array ConfigManager::get_hosts() {
	Array hosts;
	int size = config->get_value("hosts", "size", 0);
	for (int i = 1; i <= size; i++) {
		Dictionary host;
		String prefix = _get_host_prefix(i);
		host["id"] = i; // 内部索引

		// 读取常见属性
		String uuid_key = prefix + "uuid";
		if (config->has_section_key("hosts", uuid_key)) {
			host["uuid"] = config->get_value("hosts", uuid_key);
			host["hostname"] = config->get_value("hosts", prefix + "hostname", "");
			host["mac"] = config->get_value("hosts", prefix + "mac", "");
			host["localaddress"] = config->get_value("hosts", prefix + "localaddress", "");
			hosts.append(host);
		}
	}
	return hosts;
}

int ConfigManager::add_host(const Dictionary &data) {
	int size = config->get_value("hosts", "size", 0);
	int new_idx = size + 1;
	config->set_value("hosts", "size", new_idx);

	update_host(new_idx, data);
	return new_idx;
}

void ConfigManager::update_host(int index, const Dictionary &data) {
	String prefix = _get_host_prefix(index);
	Array keys = data.keys();
	for (int i = 0; i < keys.size(); i++) {
		String key = keys[i];
		if (key != "id") {
			config->set_value("hosts", prefix + key, data[key]);
		}
	}
	save_config();
}

void ConfigManager::remove_host(int index) {
	if (!config->has_section("hosts"))
		return;

	int size = config->get_value("hosts", "size", 0);
	if (index < 1 || index > size)
		return;

	// 完整实现：收集所有数据并在内存中重排，然后覆写
	PackedStringArray keys = config->get_section_keys("hosts");
	Dictionary new_data;

	for (const String &key : keys) {
		// 解析键结构：索引\后缀
		int slash_pos = key.find("\\");
		bool processed = false;

		if (slash_pos > 0) {
			String index_str = key.substr(0, slash_pos);
			if (index_str.is_valid_int()) {
				int key_idx = index_str.to_int();
				String suffix = key.substr(slash_pos); // 包含反斜杠

				if (key_idx == index) {
					// 目标删除项：跳过（不添加到新数据中）
					processed = true;
				} else if (key_idx > index) {
					// 后续项：索引前移
					String new_key = String::num_int64(key_idx - 1) + suffix;
					new_data[new_key] = config->get_value("hosts", key);
					processed = true;
				} else {
					// 之前的项：保持不变
					new_data[key] = config->get_value("hosts", key);
					processed = true;
				}
			}
		}

		if (!processed) {
			if (key == "size") {
				new_data[key] = size - 1;
			} else {
				// 保留其他非索引键
				new_data[key] = config->get_value("hosts", key);
			}
		}
	}

	// 擦除旧片段并写入新数据
	config->erase_section("hosts");
	Array new_keys = new_data.keys();
	for (int i = 0; i < new_keys.size(); i++) {
		String k = new_keys[i];
		config->set_value("hosts", k, new_data[k]);
	}
	save_config();
}

Array ConfigManager::get_apps(int host_index) {
	Array apps;
	String base_prefix = _get_host_prefix(host_index);
	int size = config->get_value("hosts", base_prefix + "apps\\size", 0);

	for (int i = 1; i <= size; i++) {
		Dictionary app;
		String prefix = _get_app_prefix(host_index, i);
		String name_key = prefix + "name";
		if (config->has_section_key("hosts", name_key)) {
			app["index"] = i;
			app["name"] = config->get_value("hosts", name_key);
			app["id"] = config->get_value("hosts", prefix + "id", 0);
			apps.append(app);
		}
	}
	return apps;
}

void ConfigManager::add_app(int host_index, const Dictionary &data) {
	String base_prefix = _get_host_prefix(host_index);
	int size = config->get_value("hosts", base_prefix + "apps\\size", 0);
	int new_idx = size + 1;
	config->set_value("hosts", base_prefix + "apps\\size", new_idx);

	String prefix = _get_app_prefix(host_index, new_idx);
	Array keys = data.keys();
	for (int i = 0; i < keys.size(); i++) {
		String key = keys[i];
		config->set_value("hosts", prefix + key, data[key]);
	}
	save_config();
}

void ConfigManager::remove_app(int host_index, int app_index) {
	String apps_prefix = _get_host_prefix(host_index) + "apps\\";
	int size = config->get_value("hosts", apps_prefix + "size", 0);

	if (app_index < 1 || app_index > size)
		return;

	PackedStringArray keys = config->get_section_keys("hosts");
	Vector<String> keys_to_erase;
	Dictionary keys_to_set;

	// 遍历所有键，找到属于该主机应用列表的键
	for (const String &key : keys) {
		if (!key.begins_with(apps_prefix))
			continue;

		// 截取掉 "host_idx\apps\" 前缀，剩余如 "1\name", "size", "2\id"
		String sub_key = key.substr(apps_prefix.length());
		int slash_pos = sub_key.find("\\");

		if (slash_pos > 0) {
			String idx_str = sub_key.substr(0, slash_pos);
			if (idx_str.is_valid_int()) {
				int current_idx = idx_str.to_int();
				String suffix = sub_key.substr(slash_pos); // "\name" 等

				if (current_idx == app_index) {
					// 目标应用：标记删除
					keys_to_erase.push_back(key);
				} else if (current_idx > app_index) {
					// 后续应用：标记删除旧键，记录新键（前移）
					keys_to_erase.push_back(key);
					String new_key = apps_prefix + String::num_int64(current_idx - 1) + suffix;
					keys_to_set[new_key] = config->get_value("hosts", key);
				}
			}
		}
	}

	// 执行变更
	for (int i = 0; i < keys_to_erase.size(); i++) {
		config->erase_section_key("hosts", keys_to_erase[i]);
	}
	Array set_keys = keys_to_set.keys();
	for (int i = 0; i < set_keys.size(); i++) {
		config->set_value("hosts", set_keys[i], keys_to_set[set_keys[i]]);
	}

	// 更新大小
	config->set_value("hosts", apps_prefix + "size", size - 1);
	save_config();
}

void ConfigManager::set_custom_data(ConfigTarget target, int host_idx, int app_idx, String key, Variant value) {
	String section;
	String final_key;

	switch (target) {
		case TARGET_GLOBAL:
			section = "General";
			final_key = key;
			break;
		case TARGET_HOST:
			section = "hosts";
			final_key = _get_host_prefix(host_idx) + key;
			break;
		case TARGET_APP:
			section = "hosts";
			final_key = _get_app_prefix(host_idx, app_idx) + key;
			break;
	}

	config->set_value(section, final_key, value);
	save_config();
}

Variant ConfigManager::get_custom_data(ConfigTarget target, int host_idx, int app_idx, String key, Variant default_value) {
	String section;
	String final_key;

	switch (target) {
		case TARGET_GLOBAL:
			section = "General";
			final_key = key;
			break;
		case TARGET_HOST:
			section = "hosts";
			final_key = _get_host_prefix(host_idx) + key;
			break;
		case TARGET_APP:
			section = "hosts";
			final_key = _get_app_prefix(host_idx, app_idx) + key;
			break;
	}

	return config->get_value(section, final_key, default_value);
}

void ConfigManager::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_config"), &ConfigManager::load_config);
	ClassDB::bind_method(D_METHOD("save_config"), &ConfigManager::save_config);
	ClassDB::bind_method(D_METHOD("get_client_keys"), &ConfigManager::get_client_keys);

	ClassDB::bind_method(D_METHOD("get_hosts"), &ConfigManager::get_hosts);
	ClassDB::bind_method(D_METHOD("add_host", "data"), &ConfigManager::add_host);
	ClassDB::bind_method(D_METHOD("update_host", "index", "data"), &ConfigManager::update_host);
	ClassDB::bind_method(D_METHOD("remove_host", "index"), &ConfigManager::remove_host);

	ClassDB::bind_method(D_METHOD("get_apps", "host_index"), &ConfigManager::get_apps);
	ClassDB::bind_method(D_METHOD("add_app", "host_index", "data"), &ConfigManager::add_app);
	ClassDB::bind_method(D_METHOD("remove_app", "host_index", "app_index"), &ConfigManager::remove_app);

	ClassDB::bind_method(D_METHOD("set_custom_data", "target", "host_idx", "app_idx", "key", "value"), &ConfigManager::set_custom_data);
	ClassDB::bind_method(D_METHOD("get_custom_data", "target", "host_idx", "app_idx", "key", "default_value"), &ConfigManager::get_custom_data, DEFVAL(Variant()));

	BIND_ENUM_CONSTANT(TARGET_GLOBAL);
	BIND_ENUM_CONSTANT(TARGET_HOST);
	BIND_ENUM_CONSTANT(TARGET_APP);
}
