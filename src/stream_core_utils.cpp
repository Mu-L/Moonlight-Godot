#include "stream_core.h"
using namespace godot;

// Moonlight 流核心：工具函数

// Android JNI Integration for Zero-Copy (SurfaceTexture)
#ifdef __ANDROID__

// On-demand JNIEnv retrieval as requested
static JNIEnv *GetJNIEnv() {
	typedef jint (*JNI_GetCreatedJavaVMs_t)(JavaVM **, jsize, jsize *);
	// Use dlsym to avoid a direct link-time dependency on JNI_GetCreatedJavaVMs
	JNI_GetCreatedJavaVMs_t jni_get_created = (JNI_GetCreatedJavaVMs_t)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
	if (!jni_get_created)
		return nullptr;

	JavaVM *vm = nullptr;
	jsize vm_count = 0;
	jint result = jni_get_created(&vm, 1, &vm_count);
	if (result != JNI_OK || vm_count == 0) {
		return nullptr;
	}
	JNIEnv *env;
	result = vm->AttachCurrentThread(&env, NULL);
	if (result != JNI_OK) {
		return nullptr;
	}
	return env;
}
#endif

String MoonlightStreamCore::_get_error_string(int error_code) {
	switch (error_code) {
		case ML_ERROR_GRACEFUL_TERMINATION:
			return "Connection terminated gracefully";
		case ML_ERROR_NO_VIDEO_TRAFFIC:
			return "Terminating connection due to lack of video traffic";
		case ML_ERROR_NO_VIDEO_FRAME:
			return "No video frame received";
		case ML_ERROR_UNEXPECTED_EARLY_TERMINATION:
			return "Unexpected early termination";
		case ML_ERROR_PROTECTED_CONTENT:
			return "Protected content detected";
		case ML_ERROR_FRAME_CONVERSION:
			return "Frame conversion error";
		default:
			if (error_code > 0)
				return "Connection error: " + String::num_int64(error_code);
			return "Unknown error (" + String::num_int64(error_code) + ")";
	}
}
