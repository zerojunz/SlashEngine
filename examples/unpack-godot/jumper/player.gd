extends CharacterBody2D

## Pixels per second.
@export var speed := 90.0
@export var jump_velocity := -260.0

@onready var jump_sound: AudioStreamPlayer = $JumpSound


func _physics_process(delta: float) -> void:
	if not is_on_floor():
		velocity += get_gravity() * delta
	if Input.is_action_just_pressed(&"jump") and is_on_floor():
		velocity.y = jump_velocity
		jump_sound.play()  # a short blip
	velocity.x = speed
	move_and_slide()
