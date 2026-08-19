#include "bsvx_world_resource.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {

void initialize_bsvx_module(ModuleInitializationLevel level) {
	if (level != MODULE_INITIALIZATION_LEVEL_SCENE) return;

	// bsvx is linked into this module, so the two cannot disagree -- but the check costs nothing
	// and turns a future "someone swapped the static archive" into a legible message instead of a
	// crash somewhere inside a struct that changed shape.
	if (bsvx_abi_version() != BSVX_ABI_VERSION) {
		UtilityFunctions::push_error(vformat("bsvx: ABI mismatch, header %d against library %d",
				static_cast<int64_t>(BSVX_ABI_VERSION), static_cast<int64_t>(bsvx_abi_version())));
		return;
	}

	GDREGISTER_CLASS(BsvxWorld);
}

void uninitialize_bsvx_module(ModuleInitializationLevel level) {
	(void)level;
}

} // namespace

extern "C" {

GDExtensionBool GDE_EXPORT bsvx_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address,
		const GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
	init_obj.register_initializer(initialize_bsvx_module);
	init_obj.register_terminator(uninitialize_bsvx_module);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init_obj.init();
}
}
