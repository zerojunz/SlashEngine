using UnityEngine;

// The heads-up display: the pause button pauses the game.
public class Hud : MonoBehaviour
{
    [SerializeField] private Transform frog;

    public void TogglePause()
    {
        Time.timeScale = Time.timeScale == 0f ? 1f : 0f;
    }
}
