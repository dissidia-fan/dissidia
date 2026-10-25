bl_info = {
    "name": "Dissidia 012 Character Tools (GMO)",
    "author": "dissidia_fan & Claude",
    "version": (0, 31, 0),
    "blender": (4, 4, 0),
    "location": "File > Import/Export, and View3D > Sidebar > Dissidia",
    "description": "Import/export Dissidia 012 [duodecim] character models, textures and animations",
    "category": "Import-Export",
}

import bpy
from bpy.props import StringProperty, BoolProperty
from bpy_extras.io_utils import ImportHelper, ExportHelper


class DFX_OT_import(bpy.types.Operator, ImportHelper):
    """Import a Dissidia 012 character model (.gmo)"""
    bl_idname = "import_scene.dissidia_gmo"
    bl_label = "Import Dissidia GMO"
    bl_options = {'UNDO'}
    filename_ext = ".gmo"
    filter_glob: StringProperty(default="*.gmo", options={'HIDDEN'})
    import_anims: BoolProperty(name="Animations", default=True,
                               description="Import every motion as an Action named by its motion ID")

    def execute(self, context):
        from . import importer
        ob = importer.import_gmo(self.filepath, self.import_anims)
        self.report({'INFO'}, f"Imported {ob.name}")
        return {'FINISHED'}


class DFX_OT_export(bpy.types.Operator, ExportHelper):
    """Export the selected Dissidia armature (and its meshes/actions) to .gmo"""
    bl_idname = "export_scene.dissidia_gmo"
    bl_label = "Export Dissidia GMO"
    filename_ext = ".gmo"
    filter_glob: StringProperty(default="*.gmo", options={'HIDDEN'})
    force_reencode: BoolProperty(name="Re-encode everything", default=False,
                                 description="Rebuild all geometry, textures and motions even if unchanged (for testing)")

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        return ob is not None and ob.type == 'ARMATURE' and 'dfx_template' in ob

    def execute(self, context):
        from . import exporter
        report = exporter.export_gmo(context.active_object, self.filepath, self.force_reencode)
        self.report({'INFO'}, report)
        return {'FINISHED'}


def _menu_import(self, context):
    self.layout.operator(DFX_OT_import.bl_idname, text="Dissidia 012 character (.gmo)")


def _menu_export(self, context):
    self.layout.operator(DFX_OT_export.bl_idname, text="Dissidia 012 character (.gmo)")


classes = (DFX_OT_import, DFX_OT_export)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    from . import panel, charmods, charmod_moves, studio_link, own_model, cape
    panel.register()
    charmods.register()
    charmod_moves.register()
    own_model.register()
    cape.register()
    studio_link.register()                               # the Move Studio's "Open in Blender" (127.0.0.1 only)
    bpy.types.TOPBAR_MT_file_import.append(_menu_import)
    bpy.types.TOPBAR_MT_file_export.append(_menu_export)


def unregister():
    from . import panel, charmods, charmod_moves, studio_link, own_model, cape
    studio_link.unregister()
    cape.unregister()
    own_model.unregister()
    charmod_moves.unregister()
    charmods.unregister()
    panel.unregister()
    bpy.types.TOPBAR_MT_file_import.remove(_menu_import)
    bpy.types.TOPBAR_MT_file_export.remove(_menu_export)
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
