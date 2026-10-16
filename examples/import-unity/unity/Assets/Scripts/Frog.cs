using UnityEngine;

// The player: walks, hops onto lily pads and croaks when it jumps.
[RequireComponent(typeof(Rigidbody2D))]
public class Frog : MonoBehaviour
{
    public float jumpSpeed = 7.5f;
    public float moveSpeed = 4f;
    [SerializeField] private AudioSource croak;
    [SerializeField] private Transform feet;

    private Rigidbody2D body;
    private int coins;

    void Awake()
    {
        body = GetComponent<Rigidbody2D>();
    }

    void Update()
    {
        float move = Input.GetAxis("Horizontal");
        body.velocity = new Vector2(move * moveSpeed, body.velocity.y);
        if (Input.GetButtonDown("Jump") && Grounded())
        {
            body.velocity = new Vector2(body.velocity.x, jumpSpeed);
            croak.Play();
        }
    }

    bool Grounded()
    {
        return Physics2D.OverlapBox(feet.position, new Vector2(0.5f, 0.125f), 0f) != null;
    }

    public void Collect(int value)
    {
        coins += value;
    }
}
