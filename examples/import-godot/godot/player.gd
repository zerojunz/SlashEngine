extends CharacterBody2D

signal coin_collected(total: int)

@export var speed := 120.0
@export var jump_velocity: float = -260.0

var coins := 0


func _physics_process(delta: float) -> void:
	if not is_on_floor():
		velocity += get_gravity() * delta
	if Input.is_action_just_pressed("jump") and is_on_floor():
		velocity.y = jump_velocity
	var direction := Input.get_axis("move_left", "move_right")
	velocity.x = direction * speed
	$AnimatedSprite2D.play("walk" if direction else "idle")
	move_and_slide()


func collect() -> void:
	coins += 1
	coin_collected.emit(coins)
