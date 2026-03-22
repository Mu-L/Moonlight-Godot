#include "register_types.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

// #include "moonlight_stream_core.h"
#include "moonlight_computer_manager.h"
#include "moonlight_config_manager.h" // Include header
#include "moonlight_godot.h"
#include "moonlight_requester.h"
#include "stream_core.h"
#include "stream_core_input_enum.h"
#include "stream_core_struct.h"

using namespace godot;

void initialize_gdextension_types(ModuleInitializationLevel p_level)
{
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	// 必须先注册父类，再注册子类
	GDREGISTER_CLASS(MoonlightGodot);
	
	GDREGISTER_CLASS(MoonlightConfigManager); // Register class
	GDREGISTER_CLASS(MoonlightRequester);
	GDREGISTER_CLASS(MoonlightComputerManager);
	GDREGISTER_CLASS(MoonlightStreamCore);
	GDREGISTER_CLASS(AudioStreamMoonlight);
	GDREGISTER_CLASS(AudioStreamPlaybackMoonlight);
	GDREGISTER_CLASS(MoonlightStreamConfigurationResource);
	GDREGISTER_CLASS(MoonlightAdditionalStreamOptions);
	GDREGISTER_CLASS(MoonlightInput);
}

void uninitialize_gdextension_types(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}

extern "C"
{
	// Initialization
	GDExtensionBool GDE_EXPORT moonlight_gd_extension_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization)
	{
		GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
		init_obj.register_initializer(initialize_gdextension_types);
		init_obj.register_terminator(uninitialize_gdextension_types);
		init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

		return init_obj.init();
	}
}