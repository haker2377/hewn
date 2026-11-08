#include "hw_cutter.h"
#include "hw_entities.h"
#include "hw_level_bake.h"
#include "hw_profile.h"
#include "hw_overlay3d.h"
#include "hw_refimage.h"
#include "hewn_mesh.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#ifdef TOOLS_ENABLED
#include "editor/hw_editor_register.h"
#endif

using namespace godot;

static void initialize_hewn(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_ABSTRACT_CLASS(HewnModifier);
		GDREGISTER_CLASS(HewnMirrorModifier);
		GDREGISTER_CLASS(HewnSolidifyModifier);
		GDREGISTER_CLASS(HewnSubdivisionModifier);
		GDREGISTER_CLASS(HewnBooleanModifier);
		GDREGISTER_CLASS(HewnArrayModifier);
		GDREGISTER_CLASS(HewnSimplifyModifier);
		GDREGISTER_CLASS(HewnRemeshModifier);
		GDREGISTER_CLASS(HewnMesh3D);
		GDREGISTER_CLASS(HewnLevel);
		GDREGISTER_CLASS(HewnProfile);
		GDREGISTER_CLASS(HewnCutter3D);
		GDREGISTER_CLASS(HewnEntity3D);
		GDREGISTER_CLASS(HewnOverlay3D);
		GDREGISTER_CLASS(HewnImage3D);
	}
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		hw_editor_register();
	}
#endif
}

static void uninitialize_hewn(ModuleInitializationLevel p_level) {
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		hw_editor_unregister();
	}
#endif
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		HewnMesh3D::free_static_resources();
		HewnOverlay3D::free_static_resources();
		HewnImage3D::free_static_resources();
	}
}

extern "C" {
GDExtensionBool GDE_EXPORT hewn_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
	init_obj.register_initializer(initialize_hewn);
	init_obj.register_terminator(uninitialize_hewn);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init_obj.init();
}
}
