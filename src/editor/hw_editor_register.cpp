#include "hw_scatter_dock.h"
#include "hw_editor_register.h"

#include "hw_editor.h"
#include "hw_quad.h"
#include "hw_prefabs.h"
#include "hw_profile_editor.h"
#include "hw_textures.h"

#include <godot_cpp/classes/editor_plugin_registration.hpp>

using namespace godot;

void hw_editor_register() {
	GDREGISTER_INTERNAL_CLASS(HewnGizmoPlugin);
	GDREGISTER_INTERNAL_CLASS(HewnEntityGizmoPlugin);
	GDREGISTER_INTERNAL_CLASS(HewnUVCanvas);
	GDREGISTER_INTERNAL_CLASS(HewnDragField);
	GDREGISTER_INTERNAL_CLASS(HewnRevertButton);
	GDREGISTER_INTERNAL_CLASS(HewnExportPlugin);
	GDREGISTER_INTERNAL_CLASS(HewnProfileView);
	GDREGISTER_INTERNAL_CLASS(HewnProfileDialog);
	GDREGISTER_INTERNAL_CLASS(HewnDock);
	GDREGISTER_INTERNAL_CLASS(HewnTextureBrowser);
	GDREGISTER_INTERNAL_CLASS(HewnPrefabBrowser);
	GDREGISTER_INTERNAL_CLASS(HewnScatterDock);
	// (the Hewn plugin is switched on / off in Project Settings > Plugins: addons/hewn/plugin.cfg runs a script extending it)
	GDREGISTER_VIRTUAL_CLASS(HewnEditorPlugin);
	GDREGISTER_INTERNAL_CLASS(HewnQuadPane);
	GDREGISTER_INTERNAL_CLASS(HewnQuadView);
	GDREGISTER_INTERNAL_CLASS(HewnQuadPlugin); // (added by the Hewn plugin as Editor Settings > Hewn > Interface > Quad View says)
}

void hw_editor_unregister() {
}
