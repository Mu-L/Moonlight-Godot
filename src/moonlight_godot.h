#pragma once

#include <godot_cpp/classes/ref_counted.hpp>

namespace godot {

class MoonlightGodot : public RefCounted {
	GDCLASS(MoonlightGodot, RefCounted)

protected:
	static void _bind_methods();

public:
	MoonlightGodot();
	~MoonlightGodot();
};

} //namespace godot
