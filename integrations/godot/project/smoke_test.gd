extends SceneTree

# Exercises the format layer end to end: author a world in memory, save it, read it back and check
# that what came out is what went in. Run with
#
#   godot --headless --path integrations/godot/project --script res://smoke_test.gd
#
# It deliberately never touches a mesh, a shader or a texture: if this passes, the format works,
# and how a renderer chooses to draw it is a separate question.

var failures := 0
var checks := 0


func check(condition: bool, what: String) -> void:
	checks += 1
	if not condition:
		failures += 1
		printerr("  FAIL  %s" % what)


func check_eq(actual, expected, what: String) -> void:
	check(actual == expected, "%s (got %s, expected %s)" % [what, actual, expected])


func _initialize() -> void:
	print("bsvx ABI version %d" % BsvxWorld.get_abi_version())
	print(BsvxWorld.get_build_info())

	test_authoring_round_trip()
	test_registry_is_the_palette()
	test_world_space_edits()
	test_validation()
	test_standalone_bytes()

	print("\n%d checks, %d failures" % [checks, failures])
	quit(1 if failures > 0 else 0)


func test_authoring_round_trip() -> void:
	print("\n[authoring round trip]")
	var world := BsvxWorld.new()
	check_eq(world.create(Vector3i(16, 16, 16), Vector3i(2, 2, 2)), OK, "create")
	check_eq(world.set_world_name("smoke"), OK, "set_world_name")
	check_eq(world.set_voxel_size(Vector3(0.25, 0.25, 0.25)), OK, "set_voxel_size")
	check_eq(world.set_registry_entry(1, 0, BsvxWorld.REGISTRY_OPAQUE | BsvxWorld.REGISTRY_COLLIDABLE), OK, "set_registry_entry")
	check_eq(world.set_voxel_name(1, "stone"), OK, "set_voxel_name")

	var region := world.add_region(Vector3i(0, 0, 0))
	check(region >= 0, "add_region")

	# Dense chunk order is x + sx * (y + sy * z); the extension does not reorder it.
	var voxels := PackedInt32Array()
	voxels.resize(16 * 16 * 16)
	voxels.fill(0)
	voxels[0] = 1
	voxels[1 + 16 * (2 + 16 * 3)] = 1
	check_eq(world.set_chunk(region, Vector3i(1, 0, 1), voxels), OK, "set_chunk")

	var path := "user://smoke_region.bvx"
	check_eq(world.save_region(path), OK, "save_region: " + world.get_last_error())

	var reloaded := BsvxWorld.new()
	check_eq(reloaded.load_region(path), OK, "load_region: " + reloaded.get_last_error())
	check_eq(reloaded.get_chunk_size(), Vector3i(16, 16, 16), "chunk size survives")
	check_eq(reloaded.get_region_size(), Vector3i(2, 2, 2), "region size survives")
	check_eq(reloaded.get_region_extent(), Vector3i(32, 32, 32), "region extent")
	check_eq(reloaded.get_voxel_name(1), "stone", "registry name survives")

	check_eq(reloaded.get_region_count(), 1, "one region")
	check(reloaded.has_chunk(0, Vector3i(1, 0, 1)), "authored chunk is present")
	check(not reloaded.has_chunk(0, Vector3i(0, 0, 0)), "untouched chunk is absent")

	var read_back := reloaded.get_chunk(0, Vector3i(1, 0, 1))
	check_eq(read_back.size(), 16 * 16 * 16, "chunk decodes to full volume")
	check_eq(read_back[0], 1, "voxel at origin")
	check_eq(read_back[1 + 16 * (2 + 16 * 3)], 1, "voxel at (1,2,3)")
	check_eq(read_back[5], 0, "air stays air")

	var info: Dictionary = reloaded.get_chunk_info(0, Vector3i(1, 0, 1))
	check_eq(info.get("non_air_count", -1), 2, "summary counts both voxels")


func test_registry_is_the_palette() -> void:
	print("\n[registry carries the palette]")
	# The whole point of the rewrite: colour lives on the registry, where every other reader of the
	# format can find it, instead of in a private section only this plugin understands.
	var world := BsvxWorld.new()
	check_eq(world.create(Vector3i(8, 8, 8), Vector3i(1, 1, 1)), OK, "create")

	var colors := PackedColorArray([Color8(110, 100, 90), Color8(90, 130, 70), Color8(200, 200, 210)])
	check_eq(world.make_palette(colors, "palette"), OK, "make_palette: " + world.get_last_error())

	var keys := world.get_registry_keys()
	check_eq(keys.size(), 3, "one registry entry per colour")
	check_eq(world.get_texture_count(), 1, "palette attached a texture archive")

	var palette := world.get_palette()
	check_eq(palette.size(), 4, "palette is indexed by voxel key")
	check_eq(palette[0], Color(0, 0, 0, 0), "key 0 is air")
	check(palette[1].is_equal_approx(Color8(110, 100, 90)), "key 1 keeps its colour")
	check(palette[3].is_equal_approx(Color8(200, 200, 210)), "key 3 keeps its colour")

	# A colour set directly on the registry must survive a save without any .btx involvement.
	var plain := BsvxWorld.new()
	check_eq(plain.create(Vector3i(8, 8, 8), Vector3i(1, 1, 1)), OK, "create")
	check_eq(plain.set_registry_entry(7, 0, BsvxWorld.REGISTRY_OPAQUE), OK, "set_registry_entry")
	check_eq(plain.set_voxel_color(7, Color8(12, 34, 56, 255)), OK, "set_voxel_color")
	var region := plain.add_region(Vector3i(0, 0, 0))
	var voxels := PackedInt32Array()
	voxels.resize(8 * 8 * 8)
	voxels.fill(0)
	voxels[0] = 7
	check_eq(plain.set_chunk(region, Vector3i(0, 0, 0), voxels), OK, "set_chunk")
	check_eq(plain.save_region("user://smoke_palette.bvx"), OK, "save_region: " + plain.get_last_error())

	var reloaded := BsvxWorld.new()
	check_eq(reloaded.load_region("user://smoke_palette.bvx"), OK, "load_region: " + reloaded.get_last_error())
	check(reloaded.get_voxel_color(7).is_equal_approx(Color8(12, 34, 56, 255)), "registry colour survives the round trip")


func test_world_space_edits() -> void:
	print("\n[world-space edits]")
	var world := BsvxWorld.new()
	check_eq(world.create(Vector3i(16, 16, 16), Vector3i(2, 2, 2)), OK, "create")
	check_eq(world.set_registry_entry(1, 0, BsvxWorld.REGISTRY_OPAQUE), OK, "set_registry_entry")

	check_eq(world.set_voxel(Vector3i(5, 6, 7), 1), OK, "set_voxel creates its region")
	check_eq(world.get_voxel(Vector3i(5, 6, 7)), 1, "get_voxel reads it back")
	check_eq(world.get_voxel(Vector3i(5, 6, 8)), 0, "neighbour is air")

	# Negative coordinates are where a hand-rolled decomposition goes wrong: C division truncates
	# toward zero where the format floors.
	check_eq(world.set_voxel(Vector3i(-1, -1, -1), 1), OK, "set_voxel below the origin")
	check_eq(world.get_voxel(Vector3i(-1, -1, -1)), 1, "negative coordinate round trips")
	var address: Dictionary = world.locate_voxel(Vector3i(-1, -1, -1))
	check_eq(address.get("region"), Vector3i(-1, -1, -1), "locate_voxel floors instead of truncating")
	check_eq(address.get("local"), Vector3i(15, 15, 15), "local coordinate wraps to the far corner")

	var written := world.fill_box(Vector3i(0, 0, 0), Vector3i(3, 3, 3), 1)
	check_eq(written, 64, "fill_box writes the whole inclusive box")

	var positions := PackedInt64Array([0, 0, 0, 3, 3, 3, 100, 100, 100])
	var keys := world.get_voxels(positions)
	check_eq(keys.size(), 3, "one key per position triple")
	check_eq(keys[0], 1, "filled corner")
	check_eq(keys[1], 1, "opposite filled corner")
	check_eq(keys[2], 0, "unauthored space reads as air")


func test_validation() -> void:
	print("\n[validation]")
	var world := BsvxWorld.new()
	check_eq(world.create(Vector3i(8, 8, 8), Vector3i(1, 1, 1)), OK, "create")
	var region := world.add_region(Vector3i(0, 0, 0))

	# Key 9 is used but never registered, which is precisely what a deep validation is for.
	var voxels := PackedInt32Array()
	voxels.resize(8 * 8 * 8)
	voxels.fill(0)
	voxels[0] = 9
	check_eq(world.set_chunk(region, Vector3i(0, 0, 0), voxels), OK, "set_chunk")

	var issues: Array = world.validate(true)
	var found := false
	for issue in issues:
		# Code 1 is VOXEL_KEY_NOT_IN_REGISTRY. It is reported at WARNING, not ERROR -- the file is
		# still readable, the key just resolves to nothing.
		if int(issue.get("code", 0)) == 1 and int(issue.get("voxel_key", 0)) == 9:
			found = true
			check(int(issue.get("severity", -1)) >= BsvxWorld.SEVERITY_WARNING, "reported at warning or above")
			check(not String(issue.get("message", "")).is_empty(), "the issue carries a message")
	check(found, "an unregistered voxel key is reported")


func test_standalone_bytes() -> void:
	print("\n[serialize to bytes]")
	# The path an exported game takes: no filesystem, just the bytes out of a pack file.
	var world := BsvxWorld.new()
	check_eq(world.create(Vector3i(8, 8, 8), Vector3i(1, 1, 1)), OK, "create")
	check_eq(world.set_registry_entry(1, 0, BsvxWorld.REGISTRY_OPAQUE), OK, "set_registry_entry")
	var region := world.add_region(Vector3i(0, 0, 0))
	var voxels := PackedInt32Array()
	voxels.resize(8 * 8 * 8)
	voxels.fill(1)
	check_eq(world.set_chunk(region, Vector3i(0, 0, 0), voxels), OK, "set_chunk")

	var bytes := world.save_region_bytes()
	check(bytes.size() > 0, "save_region_bytes produced something")

	var reloaded := BsvxWorld.new()
	check_eq(reloaded.load_region_bytes(bytes), OK, "load_region_bytes: " + reloaded.get_last_error())
	var read_back := reloaded.get_chunk(0, Vector3i(0, 0, 0))
	check_eq(read_back.size(), 8 * 8 * 8, "chunk survives the buffer round trip")
	check_eq(read_back[17], 1, "and so do its voxels")
