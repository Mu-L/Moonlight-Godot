#pragma once

#include "moonlight_godot.h"
#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/crypto.hpp>
#include <godot_cpp/classes/crypto_key.hpp>
#include <godot_cpp/classes/x509_certificate.hpp>

namespace godot {

class ConfigManager : public MoonlightGodot {
	GDCLASS(ConfigManager, MoonlightGodot)

private:
	Ref<ConfigFile> config;
	String config_path;

	void _check_and_create_certs();
	String _format_pem_for_qt(String pem);
	String _parse_pem_from_qt(String qt_pem);
	String _get_host_prefix(int index);
	String _get_app_prefix(int host_index, int app_index);
	void _reindex_hosts();
	void _reindex_apps(int host_index);

protected:
	static void _bind_methods();

public:
	enum ConfigTarget {
		TARGET_GLOBAL,
		TARGET_HOST,
		TARGET_APP
	};

	ConfigManager();
	ConfigManager(String custom_config_path);
	~ConfigManager();

	static Ref<ConfigManager> create_config(String custom_config_path = "");

	void initialize(String custom_config_path = "");

	void load_config();
	void save_config();

	// Certificates
	Dictionary get_client_keys();
	Dictionary get_client_cert_paths(); // New method

	// Hosts
	Array get_hosts();
	int add_host(const Dictionary &data);
	void update_host(int index, const Dictionary &data);
	void remove_host(int index);

	// Apps
	Array get_apps(int host_index);
	void add_app(int host_index, const Dictionary &data);
	void remove_app(int host_index, int app_index);

	// Custom
	void set_custom_data(ConfigTarget target, int host_idx, int app_idx, String key, Variant value);
	Variant get_custom_data(ConfigTarget target, int host_idx, int app_idx, String key, Variant default_value = Variant());
};

} // namespace godot

VARIANT_ENUM_CAST(ConfigManager::ConfigTarget);
