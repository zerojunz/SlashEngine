using UnityEngine;

// Keeps the camera on its target, easing towards it.
public class CameraFollow : MonoBehaviour
{
    public Transform target;
    public float smoothing = 0.2f;

    void LateUpdate()
    {
        Vector3 goal = new Vector3(target.position.x, target.position.y, transform.position.z);
        transform.position = Vector3.Lerp(transform.position, goal, smoothing);
    }
}
