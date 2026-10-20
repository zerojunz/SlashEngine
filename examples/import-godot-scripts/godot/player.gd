extends Node2D
## Walks to its goal and counts the coins it is given.

signal coin_collected(total: int)

const STEP_LIMIT := 4.0

## Pixels a second.
@export var speed: float = 60.0
@export var goal: Node2D
var coins := 0


func _ready() -> void:
	print("ready with ", coins, " coins")


func _physics_process(delta: float) -> void:
	position.x = move_toward(position.x, goal.position.x, min(speed * delta, STEP_LIMIT))


func _on_coin_pressed() -> void:
	coins += 1
	coin_collected.emit(coins)
