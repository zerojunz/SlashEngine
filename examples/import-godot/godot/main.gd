extends Node2D

@export var coins_to_win := 3


func _on_coin_body_entered(body: Node2D) -> void:
	if body.has_method("collect"):
		body.collect()
		$Coin.queue_free()
		$HUD/Score.text = "Coins: %d" % body.coins


func _on_timer_timeout() -> void:
	get_tree().reload_current_scene()
