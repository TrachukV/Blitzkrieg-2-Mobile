extends Node3D

const SNAPSHOT_PATH := "res://data/presentation_snapshot.json"
const API_VERSION := 3

@onready var camera_rig: Node3D = $CameraRig
@onready var camera: Camera3D = $CameraRig/Camera3D
@onready var status_label: Label = $Hud/Panel/Status

var world_scale := 1.0
var world_center := Vector3.ZERO


func _ready() -> void:
	_configure_environment()
	var snapshot := _load_snapshot()
	if snapshot.is_empty():
		status_label.text = "Snapshot load failed\nSee Godot output for details."
		return
	if int(snapshot.get("api_version", -1)) != API_VERSION:
		push_error("Unsupported BK2 presentation API version")
		return

	var center: Array = snapshot.get("center", [0.0, 0.0, 0.0])
	world_center = Vector3(float(center[0]), float(center[2]), float(center[1]))
	world_scale = 80.0 / max(float(snapshot.get("world_size", 1.0)), 1.0)

	var terrain: Dictionary = snapshot.get("terrain", {})
	var world: Dictionary = snapshot.get("world", {})
	var entities: Array = snapshot.get("entities", [])
	_build_mesh("Terrain", terrain, true)
	_build_mesh("StaticObjects", world, false)
	_build_entities(entities)

	camera.look_at(Vector3.ZERO, Vector3.UP)
	var mission_id := str(snapshot.get("mission_id", "<unknown>"))
	status_label.text = (
		"Hybrid renderer spike — API v%d\n%s\nterrain %d verts | entities %d"
		% [
			API_VERSION,
			mission_id,
			terrain.get("vertices", []).size(),
			entities.size(),
		]
	)
	print(
		"BK2_GODOT_BRIDGE_OK api=%d mission=%s terrain_vertices=%d entities=%d"
		% [
			API_VERSION,
			mission_id,
			terrain.get("vertices", []).size(),
			entities.size(),
		]
	)
	if "--capture-smoke" in OS.get_cmdline_user_args():
		_capture_smoke.call_deferred()


func _process(delta: float) -> void:
	var input_vector := Input.get_vector(
		"camera_left",
		"camera_right",
		"camera_forward",
		"camera_back"
	)
	if input_vector.length_squared() > 0.0:
		camera_rig.position += Vector3(input_vector.x, 0.0, input_vector.y) * delta * 18.0


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.pressed:
		if event.button_index == MOUSE_BUTTON_WHEEL_UP:
			camera.position *= 0.9
			camera.look_at(camera_rig.global_position, Vector3.UP)
		elif event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			camera.position *= 1.1
			camera.look_at(camera_rig.global_position, Vector3.UP)


func _load_snapshot() -> Dictionary:
	if not FileAccess.file_exists(SNAPSHOT_PATH):
		push_error("Missing presentation snapshot: %s" % SNAPSHOT_PATH)
		return {}
	var file := FileAccess.open(SNAPSHOT_PATH, FileAccess.READ)
	if file == null:
		push_error("Could not open presentation snapshot")
		return {}
	var parsed: Variant = JSON.parse_string(file.get_as_text())
	if not parsed is Dictionary:
		push_error("Presentation snapshot is not a JSON object")
		return {}
	return parsed


func _configure_environment() -> void:
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.12, 0.16, 0.19)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.55, 0.62, 0.68)
	environment.ambient_light_energy = 0.72
	$WorldEnvironment.environment = environment


func _build_mesh(node_name: String, data: Dictionary, is_terrain: bool) -> void:
	var vertices: Array = data.get("vertices", [])
	var indices: Array = data.get("indices", [])
	if vertices.is_empty() or indices.is_empty():
		return

	var surface := SurfaceTool.new()
	surface.begin(Mesh.PRIMITIVE_TRIANGLES)
	for raw_index: Variant in indices:
		var vertex: Array = vertices[int(raw_index)]
		surface.set_color(_abgr_to_color(int(vertex[5])))
		surface.set_uv(Vector2(float(vertex[3]), float(vertex[4])))
		surface.add_vertex(_to_godot_position(vertex))
	surface.generate_normals()

	var mesh_instance := MeshInstance3D.new()
	mesh_instance.name = node_name
	mesh_instance.mesh = surface.commit()
	var material := StandardMaterial3D.new()
	material.vertex_color_use_as_albedo = true
	material.roughness = 0.92 if is_terrain else 0.72
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	mesh_instance.material_override = material
	add_child(mesh_instance)


func _build_entities(entities: Array) -> void:
	var root := Node3D.new()
	root.name = "DynamicEntities"
	add_child(root)
	for raw: Variant in entities:
		var entity: Dictionary = raw
		var marker := MeshInstance3D.new()
		marker.name = "Entity_%s" % entity.get("id", 0)
		var body := CylinderMesh.new()
		body.top_radius = 0.32
		body.bottom_radius = 0.58
		body.height = 1.5
		marker.mesh = body
		marker.position = _to_godot_position(entity.get("position", [0.0, 0.0, 0.0]))
		marker.position.y += 0.75
		marker.rotation.y = -float(entity.get("heading", 0.0))
		var material := StandardMaterial3D.new()
		material.albedo_color = _player_color(int(entity.get("player", 0)))
		material.roughness = 0.62
		marker.material_override = material
		root.add_child(marker)


func _to_godot_position(vertex: Array) -> Vector3:
	var source := Vector3(float(vertex[0]), float(vertex[2]), float(vertex[1]))
	return (source - world_center) * world_scale


func _abgr_to_color(value: int) -> Color:
	var red := float(value & 0xff) / 255.0
	var green := float((value >> 8) & 0xff) / 255.0
	var blue := float((value >> 16) & 0xff) / 255.0
	var alpha := float((value >> 24) & 0xff) / 255.0
	return Color(red, green, blue, alpha)


func _player_color(player: int) -> Color:
	var colors := [
		Color("d5d1c7"),
		Color("df5b4f"),
		Color("4aa3df"),
		Color("67b85a"),
		Color("d98b3a"),
		Color("b56ad9"),
	]
	return colors[posmod(player, colors.size())]


func _capture_smoke() -> void:
	await RenderingServer.frame_post_draw
	var output_dir := ProjectSettings.globalize_path("res://artifacts")
	DirAccess.make_dir_recursive_absolute(output_dir)
	var output_path := output_dir.path_join("godot_bridge_smoke.png")
	var result := get_viewport().get_texture().get_image().save_png(output_path)
	if result != OK:
		push_error("Could not save smoke capture: %s" % error_string(result))
		get_tree().quit(1)
		return
	print("BK2_GODOT_CAPTURE_OK path=%s" % output_path)
	get_tree().quit()
