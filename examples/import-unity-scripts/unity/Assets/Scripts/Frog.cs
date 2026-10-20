using UnityEngine;
using UnityEngine.Events;

/// <summary>Drifts to its lily pad as it falls, picking up the coins on the way.</summary>
[RequireComponent(typeof(Rigidbody2D))]
public class Frog : MonoBehaviour
{
    [Tooltip("Units a second, across.")]
    [Range(0f, 5f)] public float speed = 1f;
    public Transform pad;
    [SerializeField] private int coins;
    public UnityEvent<int> onLanded;

    private int landings;

    void Start()
    {
        Debug.Log($"Frog ready with {coins} coins");
    }

    void Update()
    {
        Vector3 position = transform.position;
        position.x = Mathf.MoveTowards(position.x, pad.position.x, speed * Time.deltaTime);
        transform.position = position;
    }

    void OnTriggerEnter2D(Collider2D other)
    {
        coins++;
        Destroy(other.gameObject);
    }

    void OnCollisionEnter2D(Collision2D collision)
    {
        landings++;
        onLanded.Invoke(coins);
    }
}
