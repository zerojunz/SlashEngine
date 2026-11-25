# Renamed names

Which components, fields, style box keys, project settings, file suffixes, commands, command params and schema kinds took SlashEngine's own names in November 2026, and how their old names still work.

- Scenes, prefabs and command params take either name. Saving, `scene format`, `engine.describe` and schemas use the new one.
- Games' C++ keeps compiling: each old name is a deprecated alias of the new type, so the compiler names the new one.
- The tables live in `engine/world/src/aliases.cpp` (`slash/world/aliases.hpp`): `current_component_name`, `current_field_name`, `upgrade_scene`, `upgrade_params`, for `project.json` `upgrade_settings` (setting values too), and `has_suffix`, `current_command_name`, `upgrade_param_names` and `current_param_value`.
- Importers write the new names; Autopilot still reports the names its agent knows.

| Old name | New name |
| --- | --- |
| `AnimatableBody2D` | `MovingBody2D` |
| `AnimatableBody3D` | `MovingBody3D` |
| `AnimatedSprite2D` | `Flipbook2D` |
| `AnimationPlayer` | `Animator` |
| `AnimationTree` | `AnimationGraph` |
| `Area2D` | `Zone2D` |
| `Area3D` | `Zone3D` |
| `AudioListener2D` | `Listener2D` |
| `AudioListener3D` | `Listener3D` |
| `AudioStreamPlayer` | `SoundPlayer` |
| `AudioStreamPlayer2D` | `SoundPlayer2D` |
| `AudioStreamPlayer3D` | `SoundPlayer3D` |
| `BoneAttachment3D` | `BoneSocket3D` |
| `BoxContainer` | `StackBox` |
| `BoxMesh` | `BoxGeometry` |
| `CCDIK3D` | `CcdIK3D` |
| `CPUParticles2D` | `Emitter2D` |
| `CPUParticles3D` | `Emitter3D` |
| `CSGBox3D` | `SolidBox3D` |
| `CSGCombiner3D` | `SolidGroup3D` |
| `CSGCylinder3D` | `SolidCylinder3D` |
| `CSGMesh3D` | `SolidMesh3D` |
| `CSGPolygon3D` | `SolidExtrusion3D` |
| `CSGSphere3D` | `SolidSphere3D` |
| `CSGTorus3D` | `SolidTorus3D` |
| `CameraAttributesPractical` | `CameraLens` |
| `CanvasLayer` | `ScreenLayer` |
| `CanvasModulate` | `AmbientTint2D` |
| `CapsuleMesh` | `CapsuleGeometry` |
| `CenterContainer` | `CenterBox` |
| `CharacterBody2D` | `CharacterMover2D` |
| `CharacterBody3D` | `CharacterMover3D` |
| `CheckButton` | `ToggleSwitch` |
| `CodeEdit` | `CodeEditor` |
| `CodeHighlighter` | `SyntaxColors` |
| `CollisionPolygon2D` | `PolygonCollider2D` |
| `CollisionShape2D` | `Collider2D` |
| `CollisionShape3D` | `Collider3D` |
| `ColorRect` | `ColorFill` |
| `ConeTwistJoint3D` | `ConeJoint3D` |
| `Control` | `Widget` |
| `CopyTransformModifier3D` | `BoneCopy3D` |
| `CylinderMesh` | `CylinderGeometry` |
| `DampedSpringJoint2D` | `SpringJoint2D` |
| `DirectionalLight2D` | `SunLight2D` |
| `DirectionalLight3D` | `SunLight3D` |
| `FABRIK3D` | `FabrikIK3D` |
| `FlowContainer` | `FlowBox` |
| `FogMaterial` | `FogVolumeLook` |
| `GPUParticles2D` | `GpuEmitter2D` |
| `GPUParticles3D` | `GpuEmitter3D` |
| `GPUParticlesAttractorBox3D` | `ParticleAttractorBox3D` |
| `GPUParticlesAttractorSphere3D` | `ParticleAttractorSphere3D` |
| `GPUParticlesAttractorVectorField3D` | `ParticleFlowField3D` |
| `GPUParticlesCollisionBox3D` | `ParticleColliderBox3D` |
| `GPUParticlesCollisionHeightField3D` | `ParticleColliderHeight3D` |
| `GPUParticlesCollisionSDF3D` | `ParticleColliderSdf3D` |
| `GPUParticlesCollisionSphere3D` | `ParticleColliderSphere3D` |
| `GaussianSplat3D` | `Splat3D` |
| `Generic6DOFJoint3D` | `SixAxisJoint3D` |
| `GridContainer` | `GridBox` |
| `GridMap` | `BlockMap3D` |
| `GrooveJoint2D` | `TrackJoint2D` |
| `HTerrain` | `Terrain3D` |
| `HairInstance3D` | `Hair3D` |
| `LightOccluder2D` | `Occluder2D` |
| `LightmapGI` | `BakedLighting3D` |
| `LightmapProbe` | `LightProbe3D` |
| `LineEdit` | `TextField` |
| `LookAtModifier3D` | `BoneLookAt3D` |
| `MarginContainer` | `MarginBox` |
| `MeshInstance3D` | `Model3D` |
| `MultiMeshInstance3D` | `ModelInstances3D` |
| `MultiplayerSpawner` | `NetSpawner` |
| `MultiplayerSynchronizer` | `NetSync` |
| `NavigationAgent2D` | `NavAgent2D` |
| `NavigationAgent3D` | `NavAgent3D` |
| `NavigationLink2D` | `NavLink2D` |
| `NavigationLink3D` | `NavLink3D` |
| `NavigationObstacle2D` | `NavObstacle2D` |
| `NavigationObstacle3D` | `NavObstacle3D` |
| `NavigationRegion2D` | `NavArea2D` |
| `NavigationRegion3D` | `NavArea3D` |
| `NinePatchRect` | `NineSlice` |
| `OccluderInstance3D` | `Occluder3D` |
| `OmniLight3D` | `PointLight3D` |
| `OptionButton` | `Dropdown` |
| `PanelContainer` | `PanelBox` |
| `PanoramaSkyMaterial` | `SkyPanorama` |
| `Path2D` | `Spline2D` |
| `Path3D` | `Spline3D` |
| `PathFollow2D` | `SplineFollower2D` |
| `PathFollow3D` | `SplineFollower3D` |
| `PhysicalBone3D` | `SimulatedBone3D` |
| `PhysicalBoneSimulator3D` | `BoneSimulation3D` |
| `PhysicalSkyMaterial` | `SkyScattering` |
| `PlaneMesh` | `PlaneGeometry` |
| `PopupMenu` | `PopupList` |
| `PrismMesh` | `PrismGeometry` |
| `ProceduralSkyMaterial` | `SkyGradient` |
| `QuadMesh` | `QuadGeometry` |
| `ScrollContainer` | `ScrollBox` |
| `SliderJoint3D` | `SlideJoint3D` |
| `SphereMesh` | `SphereGeometry` |
| `StandardMaterial3D` | `Surface3D` |
| `TextEdit` | `TextArea` |
| `TextureButton` | `ImageButton` |
| `TextureRect` | `Picture` |
| `TileMapLayer` | `TileLayer2D` |
| `TorusMesh` | `TorusGeometry` |
| `VehicleBody3D` | `WheeledBody3D` |
| `VehicleWheel3D` | `Wheel3D` |
| `VideoStreamPlayer` | `Video` |
| `VoxelGI` | `VoxelLighting3D` |
| `WorldEnvironment` | `Environment3D` |
| `XRFaceModifier3D` | `XRFace3D` |

## Fields

Fields renamed at the same time. Scenes, prefabs, command params that hold components, animation track paths and replicated properties take either name.

| Old name | New name | Components |
| --- | --- | --- |
| `action_mode` | `press_on` | `Button`, `CheckBox`, `Dropdown` and 3 more |
| `angular_damp_space_override` | `angular_damp_mode` | `Zone2D`, `Zone3D` |
| `area_mask` | `zone_mask` | `SoundPlayer2D`, `SoundPlayer3D` |
| `audio_bus_override` | `bus_override` | `Zone2D`, `Zone3D` |
| `autowrap_mode` | `wrap` | `Button`, `CheckBox`, `Dropdown` and 4 more |
| `axis_stretch_horizontal` | `stretch_horizontal` | `NineSlice` |
| `axis_stretch_vertical` | `stretch_vertical` | `NineSlice` |
| `billboard_mode` | `billboard` | `Surface3D` |
| `button_pressed` | `pressed` | `Button`, `CheckBox`, `Dropdown` and 3 more |
| `clip_contents` | `clip_children` | `Widget` |
| `collision_base_size` | `collision_size` | `GpuEmitter2D`, `GpuEmitter3D` |
| `contact_monitor` | `report_contacts` | `RigidBody2D`, `RigidBody3D` |
| `cull_mask` | `layer_filter` | `Camera3D`, `Decal`, `ParticleAttractorBox3D` and 7 more |
| `custom_minimum_size` | `min_size` | `Widget` |
| `draw_center` | `fill_center` | `NineSlice` |
| `draw_pass_1` | `mesh_1` | `GpuEmitter3D` |
| `draw_pass_2` | `mesh_2` | `GpuEmitter3D` |
| `draw_pass_3` | `mesh_3` | `GpuEmitter3D` |
| `draw_pass_4` | `mesh_4` | `GpuEmitter3D` |
| `draw_passes` | `mesh_count` | `GpuEmitter3D` |
| `explosiveness` | `burst` | `Emitter2D`, `Emitter3D`, `GpuEmitter2D`, `GpuEmitter3D` |
| `external_skeleton` | `skeleton_entity` | `BoneSocket3D` |
| `fixed_fps` | `fixed_rate` | `GpuEmitter2D`, `GpuEmitter3D` |
| `focus_neighbor_bottom` | `focus_bottom` | `Widget` |
| `focus_neighbor_left` | `focus_left` | `Widget` |
| `focus_neighbor_right` | `focus_right` | `Widget` |
| `focus_neighbor_top` | `focus_top` | `Widget` |
| `fract_delta` | `smooth_emission` | `GpuEmitter2D`, `GpuEmitter3D` |
| `freeze` | `frozen` | `RigidBody2D`, `RigidBody3D` |
| `freeze_mode` | `frozen_mode` | `RigidBody2D`, `RigidBody3D` |
| `gi_lightmap_texel_scale` | `lightmap_texel_scale` | `Model3D` |
| `gi_mode` | `lighting_mode` | `Model3D` |
| `gravity_space_override` | `gravity_mode` | `Zone2D`, `Zone3D` |
| `hframes` | `columns` | `Emitter2D`, `GpuEmitter2D`, `Sprite2D` |
| `ignore_occlusion_culling` | `skip_occlusion` | `Model3D` |
| `ignore_texture_size` | `ignore_image_size` | `ImageButton` |
| `keep_pressed_outside` | `hold_outside` | `Button`, `CheckBox`, `Dropdown` and 3 more |
| `light_cull_mask` | `lit_layers` | `PointLight3D`, `SpotLight3D`, `SunLight3D` |
| `light_mask` | `light_layers` | `Cloth2D`, `Emitter2D`, `Fluid2D` and 7 more |
| `linear_damp_space_override` | `linear_damp_mode` | `Zone2D`, `Zone3D` |
| `local_coords` | `local_space` | `Emitter2D`, `Emitter3D`, `GpuEmitter2D`, `GpuEmitter3D` |
| `material_override` | `material` | `Model3D` |
| `max_db` | `max_volume_db` | `SoundPlayer3D` |
| `mesh_library` | `blocks` | `BlockMap3D` |
| `modulate` | `tint` | `Decal`, `Fluid2D`, `Foliage2D` and 6 more |
| `monitorable` | `detectable` | `Zone2D`, `Zone3D` |
| `monitoring` | `detects` | `Zone2D`, `Zone3D` |
| `motion_mode` | `movement` | `CharacterMover2D`, `CharacterMover3D` |
| `mouse_filter` | `pointer_mode` | `Widget` |
| `navigation_mesh` | `navmesh` | `NavArea3D` |
| `navigation_polygon` | `navmesh` | `NavArea2D` |
| `node_a` | `body_a` | `ConeJoint3D`, `HingeJoint3D`, `PinJoint2D` and 5 more |
| `node_b` | `body_b` | `ConeJoint3D`, `HingeJoint3D`, `PinJoint2D` and 5 more |
| `occluder_light_mask` | `light_layers` | `Occluder2D` |
| `origin_external_node` | `origin_entity` | `BoneLookAt3D` |
| `patch_margin_bottom` | `border_bottom` | `NineSlice` |
| `patch_margin_left` | `border_left` | `NineSlice` |
| `patch_margin_right` | `border_right` | `NineSlice` |
| `patch_margin_top` | `border_top` | `NineSlice` |
| `path_node` | `path` | `SolidExtrusion3D` |
| `physics_material_override` | `physics_material` | `MovingBody2D`, `MovingBody3D`, `RigidBody2D` and 4 more |
| `pitch_scale` | `pitch` | `SoundPlayer`, `SoundPlayer2D`, `SoundPlayer3D` |
| `pivot_offset` | `pivot` | `Widget` |
| `process_material` | `particle_rules` | `GpuEmitter2D`, `GpuEmitter3D` |
| `range_item_cull_mask` | `lit_layers` | `PointLight2D`, `SunLight2D` |
| `region_rect` | `region` | `NineSlice` |
| `sdfgi_bounce_feedback` | `field_lighting_bounce_feedback` | `Environment3D` |
| `sdfgi_cascades` | `field_lighting_cascades` | `Environment3D` |
| `sdfgi_enabled` | `field_lighting_enabled` | `Environment3D` |
| `sdfgi_energy` | `field_lighting_energy` | `Environment3D` |
| `sdfgi_min_cell_size` | `field_lighting_min_cell_size` | `Environment3D` |
| `sdfgi_normal_bias` | `field_lighting_normal_bias` | `Environment3D` |
| `sdfgi_probe_bias` | `field_lighting_probe_bias` | `Environment3D` |
| `sdfgi_read_sky_light` | `field_lighting_read_sky_light` | `Environment3D` |
| `sdfgi_use_occlusion` | `field_lighting_use_occlusion` | `Environment3D` |
| `sdfgi_y_scale` | `field_lighting_y_scale` | `Environment3D` |
| `self_modulate` | `own_tint` | `Widget` |
| `shading_mode` | `shading` | `Surface3D` |
| `shadow_item_cull_mask` | `shadow_layers` | `PointLight2D`, `SunLight2D` |
| `size_flags_horizontal` | `sizing_horizontal` | `Widget` |
| `size_flags_stretch_ratio` | `stretch_ratio` | `Widget` |
| `size_flags_vertical` | `sizing_vertical` | `Widget` |
| `sky_cover_modulate` | `sky_cover_tint` | `SkyGradient` |
| `subsurf_scatter_enabled` | `subsurface_enabled` | `Surface3D` |
| `subsurf_scatter_skin_mode` | `subsurface_skin_mode` | `Surface3D` |
| `subsurf_scatter_strength` | `subsurface_strength` | `Surface3D` |
| `subsurf_scatter_transmittance_boost` | `subsurface_transmittance_boost` | `Surface3D` |
| `subsurf_scatter_transmittance_color` | `subsurface_transmittance_color` | `Surface3D` |
| `subsurf_scatter_transmittance_depth` | `subsurface_transmittance_depth` | `Surface3D` |
| `subsurf_scatter_transmittance_enabled` | `subsurface_transmittance_enabled` | `Surface3D` |
| `target_node` | `target` | `BoneLookAt3D` |
| `text_overrun_behavior` | `overflow` | `Button`, `CheckBox`, `Dropdown` and 3 more |
| `texture_filter` | `filter` | `Cloth2D`, `Emitter2D`, `Foliage2D` and 7 more |
| `texture_repeat` | `repeat` | `Surface3D` |
| `theme_override_colors` | `theme_colors` | `Widget` |
| `theme_override_constants` | `theme_constants` | `Widget` |
| `theme_override_font_sizes` | `theme_font_sizes` | `Widget` |
| `theme_override_fonts` | `theme_fonts` | `Widget` |
| `theme_override_icons` | `theme_icons` | `Widget` |
| `theme_override_styles` | `theme_styles` | `Widget` |
| `toggle_mode` | `toggles` | `Button`, `CheckBox`, `Dropdown` and 3 more |
| `tree_root` | `root` | `AnimationGraph` |
| `unit_size` | `reference_distance` | `SoundPlayer3D` |
| `up_direction` | `up` | `CharacterMover2D`, `CharacterMover3D` |
| `uv1_offset` | `uv_offset` | `Surface3D` |
| `uv1_scale` | `uv_scale` | `Surface3D` |
| `vframes` | `rows` | `Emitter2D`, `GpuEmitter2D`, `Sprite2D` |
| `z_as_relative` | `sort_order_relative` | `Cloth2D`, `Emitter2D`, `Fluid2D` and 9 more |
| `z_index` | `sort_order` | `Cloth2D`, `Emitter2D`, `Fluid2D` and 9 more |

## Style box keys

Keys of the [style boxes](scene-format.md#style-boxes) in themes' `styles` and controls' `theme_styles`. Loading reads an old key as its new one, sides included (`expand_margin_left` is `outset_left`); where both are given, the new one wins (`ui::upgrade_style_box`, `ui::style_box_aliases`).

| Old key | New key |
| --- | --- |
| `axis_stretch_horizontal` | `stretch_horizontal` |
| `axis_stretch_vertical` | `stretch_vertical` |
| `bg_color` | `background_color` |
| `corner_detail` | `corner_segments` |
| `draw_center` | `fill_center` |
| `expand_margin` | `outset` |
| `modulate_color` | `tint` |
| `region_rect` | `region` |
| `texture_margin` | `slice_margin` |

## Project settings

Settings that moved in `project.json`. Loading moves a setting at its old place to its new one (`world::upgrade_settings`); where both are given, the new one wins. Validation errors name the new place.

| Old place | New place |
| --- | --- |
| `audio.channel_disable_threshold_db` | `audio.silence_threshold_db` |
| `audio.channel_disable_time` | `audio.silence_seconds` |
| `audio.enable_input` | `audio.input_enabled` |
| `gui.allow_system_fallback` | `ui.system_font_fallback` |
| `gui.button_shortcut_feedback_highlight_time` | `ui.shortcut_flash_seconds` |
| `gui.incremental_search_max_interval_msec` | `ui.type_search_ms` |
| `gui.text_edit_idle_detect_sec` | `ui.text_edit_merge_seconds` |
| `gui.text_edit_undo_stack_max_size` | `ui.text_edit_undo_limit` |
| `gui.tooltip_delay_sec` | `ui.tooltip_delay_seconds` |
| `gui.tooltip_position_offset` | `ui.tooltip_offset` |
| `gui` | `ui` |
| `input_devices.pointing.emulate_mouse_from_touch` | `pointer.mouse_from_touch` |
| `input_devices.pointing.emulate_touch_from_mouse` | `pointer.touch_from_mouse` |
| `internationalization.locale` | `localization` |
| `internationalization.pseudolocalization.use_pseudolocalization` | `localization.pseudo.enabled` |
| `internationalization.pseudolocalization` | `localization.pseudo` |
| `internationalization.rendering.force_right_to_left_layout_direction` | `localization.force_right_to_left` |
| `internationalization.rendering.root_node_layout_direction` | `localization.layout_direction` |
| `navigation_2d.merge_rasterizer_cell_scale` | `navigation_2d.merge_cell_scale` |
| `navigation_2d.use_edge_connections` | `navigation_2d.connect_edges` |
| `navigation_3d.merge_rasterizer_cell_scale` | `navigation_3d.merge_cell_scale` |
| `navigation_3d.use_edge_connections` | `navigation_3d.connect_edges` |
| `physics_3d.areas_detect_static_bodies` | `physics_3d.zones_detect_static_bodies` |
| `rendering.anti_aliasing.quality.use_taa` | `rendering.anti_aliasing.taa` |
| `rendering.camera.depth_of_field.depth_of_field_bokeh_quality` | `rendering.depth_of_field.bokeh_quality` |
| `rendering.camera.depth_of_field.depth_of_field_bokeh_shape` | `rendering.depth_of_field.bokeh_shape` |
| `rendering.camera.depth_of_field.depth_of_field_use_jitter` | `rendering.depth_of_field.jitter` |
| `rendering.environment.screen_space_reflection` | `rendering.ssr` |
| `rendering.environment.ssao` | `rendering.ssao` |
| `rendering.environment.subsurface_scattering` | `rendering.subsurface` |
| `rendering.environment.volumetric_fog` | `rendering.volumetric_fog` |
| `rendering.global_illumination.sdfgi` | `rendering.field_lighting` |
| `rendering.global_illumination.voxel_gi.quality` | `rendering.voxel_lighting.quality` |
| `rendering.lightmapping` | `rendering.lightmaps` |
| `rendering.lights_and_shadows.directional_shadow.size` | `rendering.shadows.directional_size` |
| `rendering.lights_and_shadows.directional_shadow.soft_shadow_filter_quality` | `rendering.shadows.directional_softness` |
| `rendering.lights_and_shadows.positional_shadow.atlas_size` | `rendering.shadows.positional_atlas_size` |
| `rendering.lights_and_shadows.positional_shadow.soft_shadow_filter_quality` | `rendering.shadows.positional_softness` |
| `rendering.lights_and_shadows.use_physical_light_units` | `rendering.physical_light_units` |
| `rendering.mesh_lod.lod_change.threshold_pixels` | `rendering.lod.threshold_pixels` |
| `rendering.occlusion_culling.occlusion_rays_per_thread` | `rendering.occlusion_culling.rays_per_thread` |
| `rendering.occlusion_culling.use_occlusion_culling` | `rendering.occlusion_culling.enabled` |
| `rendering.reflections.reflection_atlas.reflection_count` | `rendering.reflections.probe_count` |
| `rendering.reflections.reflection_atlas.reflection_size` | `rendering.reflections.probe_size` |
| `rendering.reflections.sky_reflections.ggx_samples` | `rendering.reflections.sky_samples` |
| `rendering.rendering_method` | `rendering.renderer` |
| `rendering.scaling.fsr_sharpness` | `rendering.scaling.sharpness` |
| `window.vsync_mode` | `window.vsync` |

Setting values that took new names (`world::setting_value_aliases`); loading reads the old value as the new one.

| Setting | Old value | New value |
| --- | --- | --- |
| `rendering.renderer` | `forward_plus` | `clustered` |
| `rendering.renderer` | `gl_compatibility` | `compatibility` |


## File suffixes

Files keep loading under their old suffix: scenes may name either, `scene validate` and `scene format` take both, and exports ship both. Tools write the new one.

| Old suffix | New suffix |
| --- | --- |
| `.meshlib.json` | `.blocks.json` |
| `.navpoly.json` | `.navmesh2d.json` |
| `.process_material.json` | `.particles.json` |
| `.voxelgi.json` | `.voxellight.json` |

## Commands

Every front end (CLI `call`, JSON-RPC, MCP, REST) still runs a command by its old name, and MCP clients by its old tool name (`csg_get`).

| Old name | New name |
| --- | --- |
| `csg.get` | `solid.get` |
| `gridmap.get` | `blockmap.get` |
| `gridmap.set` | `blockmap.set` |
| `voxel_gi.bake` | `voxel_lighting.bake` |

## Command params

Params a command still takes under their old names (`world::param_name_aliases`); where both are given, the new one wins. Results use the new names.

| Command | Old param | New param |
| --- | --- | --- |
| `display.set` | `vsync_mode` | `vsync` |
| `display.set` | `scaling.fsr_sharpness` | `scaling.sharpness` |

## Schema kinds

`schema.get`'s `kind` and `slash-cli schema` take either name.

| Old kind | New kind |
| --- | --- |
| `meshlib` | `blocks` |
| `navpoly` | `navmesh2d` |
| `process_material` | `particles` |
| `voxel_gi` | `voxel_light` |
