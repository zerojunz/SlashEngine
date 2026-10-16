using UnityEngine;

// A coin the frog collects by touching it.
public class Coin : MonoBehaviour
{
    public int value = 1;

    void OnTriggerEnter2D(Collider2D other)
    {
        if (other.TryGetComponent(out Frog frog))
        {
            frog.Collect(value);
            Destroy(gameObject);
        }
    }
}
