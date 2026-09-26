class_name CameraRig
extends Node3D
## God-view orbit camera: pan (WASD / middle drag), rotate (Q/E / right drag),
## zoom (wheel). Movement is smoothed; nothing here affects the simulation.

var camera: Camera3D
var target := Vector3.ZERO
var yaw := 0.7
var pitch := deg_to_rad(52.0)
var distance := 140.0

var _t_target := Vector3.ZERO
var _t_yaw := 0.7
var _t_pitch := deg_to_rad(52.0)
var _t_distance := 140.0
var _rotating := false
var _panning := false

const MIN_DIST := 8.0
const MAX_DIST := 900.0


func _ready() -> void:
	camera = Camera3D.new()
	camera.fov = 50.0
	camera.near = 0.3
	camera.far = 6000.0
	add_child(camera)
	_apply()


func focus(p: Vector3, dist: float = -1.0, instant := false) -> void:
	_t_target = p
	if dist > 0.0:
		_t_distance = dist
	if instant:
		target = _t_target
		distance = _t_distance
		yaw = _t_yaw
		pitch = _t_pitch
		_apply()


func set_view(p: Vector3, yaw_deg: float, pitch_deg: float, dist: float) -> void:
	_t_target = p
	_t_yaw = deg_to_rad(yaw_deg)
	_t_pitch = deg_to_rad(pitch_deg)
	_t_distance = dist
	focus(p, dist, true)


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		var mb := event as InputEventMouseButton
		if mb.button_index == MOUSE_BUTTON_WHEEL_UP and mb.pressed:
			_t_distance = clampf(_t_distance * 0.88, MIN_DIST, MAX_DIST)
		elif mb.button_index == MOUSE_BUTTON_WHEEL_DOWN and mb.pressed:
			_t_distance = clampf(_t_distance * 1.14, MIN_DIST, MAX_DIST)
		elif mb.button_index == MOUSE_BUTTON_RIGHT:
			_rotating = mb.pressed
		elif mb.button_index == MOUSE_BUTTON_MIDDLE:
			_panning = mb.pressed
	elif event is InputEventMouseMotion:
		var mm := event as InputEventMouseMotion
		if _rotating:
			_t_yaw -= mm.relative.x * 0.006
			_t_pitch = clampf(_t_pitch + mm.relative.y * 0.005, deg_to_rad(12.0), deg_to_rad(88.0))
		elif _panning:
			var k := _t_distance * 0.0022
			_t_target += (-_right() * mm.relative.x + _forward() * mm.relative.y) * k


func _right() -> Vector3:
	return Vector3(cos(_t_yaw), 0.0, -sin(_t_yaw))


func _forward() -> Vector3:
	return Vector3(-sin(_t_yaw), 0.0, -cos(_t_yaw))


func _process(delta: float) -> void:
	var move := Vector2.ZERO
	if not _text_focus():
		if Input.is_key_pressed(KEY_W) or Input.is_key_pressed(KEY_UP):
			move.y += 1.0
		if Input.is_key_pressed(KEY_S) or Input.is_key_pressed(KEY_DOWN):
			move.y -= 1.0
		if Input.is_key_pressed(KEY_A) or Input.is_key_pressed(KEY_LEFT):
			move.x -= 1.0
		if Input.is_key_pressed(KEY_D) or Input.is_key_pressed(KEY_RIGHT):
			move.x += 1.0
		if Input.is_key_pressed(KEY_Q):
			_t_yaw += delta * 1.6
		if Input.is_key_pressed(KEY_E):
			_t_yaw -= delta * 1.6
	if move != Vector2.ZERO:
		var spd := _t_distance * 0.9 + 20.0
		_t_target += (_right() * move.x + _forward() * move.y).normalized() * spd * delta
	var k := 1.0 - exp(-delta * 10.0)
	target = target.lerp(_t_target, k)
	yaw = lerp_angle(yaw, _t_yaw, k)
	pitch = lerpf(pitch, _t_pitch, k)
	distance = lerpf(distance, _t_distance, k)
	_apply()


func _text_focus() -> bool:
	var f := get_viewport().gui_get_focus_owner()
	return f is LineEdit or f is TextEdit


func _apply() -> void:
	var off := Vector3(sin(yaw) * cos(pitch), sin(pitch), cos(yaw) * cos(pitch)) * distance
	camera.global_position = target + off
	camera.look_at(target, Vector3.UP)
