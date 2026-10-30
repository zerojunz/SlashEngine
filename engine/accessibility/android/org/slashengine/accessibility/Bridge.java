package org.slashengine.accessibility;

import android.app.Activity;
import android.content.Context;
import android.graphics.Rect;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewParent;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityManager;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeInfo.AccessibilityAction;
import android.view.accessibility.AccessibilityNodeInfo.RangeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * TalkBack's side of a game's controls (slash/accessibility/mobile.hpp, src/android.cpp): the
 * accessibility tree's nodes as virtual views of an AccessibilityNodeProvider on SDL's surface,
 * as AccessKit's Android adapter makes them. The game's thread hands it each update's changes as
 * JSON (apply) and takes what TalkBack asked of the controls (take); Android asks for the views
 * on its UI thread, so both sides go through one lock.
 */
public final class Bridge {
    private static final String WINDOW = "1";

    // A node as the game last sent it.
    private static final class Element {
        final int virtualId;
        String parent = WINDOW;
        JSONObject fields = new JSONObject();
        // [x0, y0, x1, y1] in the game's viewport, or null.
        double[] bounds;
        List<String> children = new ArrayList<>();

        Element(int virtualId) {
            this.virtualId = virtualId;
        }
    }

    private static final Object LOCK = new Object();
    private static Bridge bridge;

    private final Activity activity;
    private final AccessibilityManager manager;
    private View host;
    // Node IDs (as strings, since Java's int can't hold them) and the virtual view IDs given them.
    private final Map<String, Element> elements = new HashMap<>();
    private final Map<Integer, String> nodes = new HashMap<>();
    private int nextVirtualId = 1;
    private String focus = WINDOW;
    private int accessibilityFocus = View.NO_ID;
    private int hovered = View.NO_ID;
    private double viewportWidth = 1.0;
    private double viewportHeight = 1.0;
    private JSONArray queue = new JSONArray();

    private Bridge(Activity activity) {
        this.activity = activity;
        this.manager =
                (AccessibilityManager) activity.getSystemService(Context.ACCESSIBILITY_SERVICE);
    }

    /** Puts the provider on the game's surface; false without an activity. */
    public static boolean start(Activity activity) {
        if (activity == null) {
            return false;
        }
        final Bridge started = new Bridge(activity);
        synchronized (LOCK) {
            bridge = started;
        }
        activity.runOnUiThread(started::attach);
        return true;
    }

    /** Takes the provider off the surface. */
    public static void stop() {
        final Bridge stopped;
        synchronized (LOCK) {
            stopped = bridge;
            bridge = null;
        }
        if (stopped != null) {
            stopped.activity.runOnUiThread(stopped::detach);
        }
    }

    /** Whether TalkBack, or another service that explores by touch, runs. */
    public static boolean active() {
        final Bridge current;
        synchronized (LOCK) {
            current = bridge;
        }
        return current != null && current.manager != null && current.manager.isEnabled()
                && current.manager.isTouchExplorationEnabled();
    }

    /** Applies android_changes' JSON, with the viewport the bounds are in. */
    public static void apply(String changes, double width, double height) {
        final Bridge current;
        synchronized (LOCK) {
            current = bridge;
        }
        if (current == null) {
            return;
        }
        try {
            current.update(new JSONObject(changes), width, height);
        } catch (JSONException error) {
            android.util.Log.e("SlashEngine", "bad accessibility update", error);
        }
    }

    /** What TalkBack asked since the last call, as a JSON array; null when it asked nothing. */
    public static String take() {
        synchronized (LOCK) {
            if (bridge == null || bridge.queue.length() == 0) {
                return null;
            }
            final String text = bridge.queue.toString();
            bridge.queue = new JSONArray();
            return text;
        }
    }

    private void attach() {
        host = findSurface(activity.getWindow().getDecorView());
        if (host == null) {
            android.util.Log.w("SlashEngine", "no surface for screen readers to read");
            return;
        }
        final Provider provider = new Provider();
        host.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_YES);
        host.setAccessibilityDelegate(new View.AccessibilityDelegate() {
            @Override
            public AccessibilityNodeProvider getAccessibilityNodeProvider(View view) {
                return provider;
            }
        });
        host.setOnHoverListener((view, event) -> hover(event));
    }

    private void detach() {
        if (host != null) {
            host.setAccessibilityDelegate(null);
            host.setOnHoverListener(null);
        }
    }

    // SDL's surface: the first SurfaceView under the activity's window.
    private static View findSurface(View view) {
        if (view instanceof SurfaceView) {
            return view;
        }
        if (view instanceof ViewGroup) {
            final ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                final View found = findSurface(group.getChildAt(i));
                if (found != null) {
                    return found;
                }
            }
        }
        return null;
    }

    private void update(JSONObject changes, double width, double height) throws JSONException {
        final List<Integer> changed = new ArrayList<>();
        int focused = View.NO_ID;
        synchronized (LOCK) {
            viewportWidth = Math.max(1.0, width);
            viewportHeight = Math.max(1.0, height);
            final JSONArray removed = changes.optJSONArray("remove");
            for (int i = 0; removed != null && i < removed.length(); i++) {
                final Element gone = elements.remove(removed.getString(i));
                if (gone != null) {
                    nodes.remove(gone.virtualId);
                }
            }
            final JSONArray set = changes.optJSONArray("set");
            for (int i = 0; set != null && i < set.length(); i++) {
                final JSONObject item = set.getJSONObject(i);
                final Element element = elementOf(item.getString("id"));
                element.fields = item;
                final JSONArray bounds = item.optJSONArray("bounds");
                element.bounds = bounds != null && bounds.length() == 4
                        ? new double[] {bounds.getDouble(0), bounds.getDouble(1),
                                bounds.getDouble(2), bounds.getDouble(3)}
                        : null;
                element.children = new ArrayList<>();
                final JSONArray children = item.optJSONArray("children");
                for (int j = 0; children != null && j < children.length(); j++) {
                    element.children.add(children.getString(j));
                }
                changed.add(element.virtualId);
            }
            // Parents from the children lists, once the elements are all there.
            for (int i = 0; set != null && i < set.length(); i++) {
                final String id = set.getJSONObject(i).getString("id");
                for (String child : elements.get(id).children) {
                    elementOf(child).parent = id;
                }
            }
            if (changes.has("focus")) {
                focus = changes.getString("focus");
                final Element element = elements.get(focus);
                focused = element != null && !WINDOW.equals(focus) ? element.virtualId : View.NO_ID;
            }
        }
        final int focusedView = focused;
        final boolean any = changes.has("remove") || !changed.isEmpty();
        activity.runOnUiThread(() -> {
            if (any) {
                final AccessibilityEvent event = event(View.NO_ID,
                        AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED);
                if (event != null) {
                    event.setContentChangeTypes(AccessibilityEvent.CONTENT_CHANGE_TYPE_SUBTREE);
                    send(event);
                }
                // Live regions, and the view TalkBack is on, are read again when they change.
                for (int virtualId : changed) {
                    if (virtualId == accessibilityFocus || liveRegion(virtualId) != 0) {
                        final AccessibilityEvent content = event(virtualId,
                                AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED);
                        if (content != null) {
                            content.setContentChangeTypes(
                                    AccessibilityEvent.CONTENT_CHANGE_TYPE_UNDEFINED);
                            send(content);
                        }
                    }
                }
            }
            // TalkBack follows the game's focus.
            if (focusedView != View.NO_ID) {
                send(event(focusedView, AccessibilityEvent.TYPE_VIEW_FOCUSED));
            }
        });
    }

    private Element elementOf(String id) {
        Element element = elements.get(id);
        if (element == null) {
            element = new Element(WINDOW.equals(id) ? View.NO_ID : nextVirtualId++);
            elements.put(id, element);
            nodes.put(element.virtualId, id);
        }
        return element;
    }

    private int liveRegion(int virtualId) {
        synchronized (LOCK) {
            final Element element = elements.get(nodes.get(virtualId));
            return element != null ? element.fields.optInt("live_region", 0) : 0;
        }
    }

    // An event from the host (View.NO_ID) or one of its virtual views; null while no service
    // listens.
    private AccessibilityEvent event(int virtualId, int type) {
        if (host == null || manager == null || !manager.isEnabled()) {
            return null;
        }
        final AccessibilityEvent event = new AccessibilityEvent(type);
        event.setPackageName(activity.getPackageName());
        if (virtualId == View.NO_ID) {
            event.setSource(host);
            event.setClassName(host.getClass().getName());
        } else {
            event.setSource(host, virtualId);
            synchronized (LOCK) {
                final Element element = elements.get(nodes.get(virtualId));
                if (element != null) {
                    event.setClassName(element.fields.optString("class_name", "android.view.View"));
                    final String text = element.fields.optString("text", "");
                    if (!text.isEmpty()) {
                        event.getText().add(text);
                    }
                }
            }
        }
        return event;
    }

    private void send(AccessibilityEvent event) {
        if (event == null || host == null) {
            return;
        }
        final ViewParent parent = host.getParent();
        if (parent != null) {
            parent.requestSendAccessibilityEvent(host, event);
        }
    }

    // Touch exploration: TalkBack turns touches into hovers, which go to the view under them.
    private boolean hover(MotionEvent motion) {
        if (manager == null || !manager.isEnabled() || !manager.isTouchExplorationEnabled()) {
            return false;
        }
        switch (motion.getActionMasked()) {
            case MotionEvent.ACTION_HOVER_ENTER:
            case MotionEvent.ACTION_HOVER_MOVE:
                setHovered(viewAt(motion.getX(), motion.getY()));
                return true;
            case MotionEvent.ACTION_HOVER_EXIT:
                setHovered(View.NO_ID);
                return true;
            default:
                return false;
        }
    }

    private void setHovered(int virtualId) {
        if (virtualId == hovered) {
            return;
        }
        final int previous = hovered;
        hovered = virtualId;
        if (virtualId != View.NO_ID) {
            send(event(virtualId, AccessibilityEvent.TYPE_VIEW_HOVER_ENTER));
        }
        if (previous != View.NO_ID) {
            send(event(previous, AccessibilityEvent.TYPE_VIEW_HOVER_EXIT));
        }
    }

    // The last visible view in the tree's order under a point of the surface: what draws on top.
    private int viewAt(float x, float y) {
        synchronized (LOCK) {
            final double vx = x * viewportWidth / Math.max(1, host.getWidth());
            final double vy = y * viewportHeight / Math.max(1, host.getHeight());
            return deepestAt(WINDOW, vx, vy);
        }
    }

    private int deepestAt(String id, double x, double y) {
        final Element element = elements.get(id);
        if (element == null) {
            return View.NO_ID;
        }
        int found = View.NO_ID;
        if (!WINDOW.equals(id)) {
            final double[] b = element.bounds;
            if (b == null || !element.fields.optBoolean("visible", true) || x < b[0] || y < b[1]
                    || x >= b[2] || y >= b[3]) {
                return View.NO_ID;
            }
            found = element.virtualId;
        }
        for (String child : element.children) {
            final int inside = deepestAt(child, x, y);
            if (inside != View.NO_ID) {
                found = inside;
            }
        }
        return found;
    }

    // An element's bounds on the screen.
    private Rect screenBounds(Element element) {
        final int[] origin = new int[2];
        host.getLocationOnScreen(origin);
        final double sx = host.getWidth() / viewportWidth;
        final double sy = host.getHeight() / viewportHeight;
        final double[] b = element.bounds != null ? element.bounds : new double[4];
        return new Rect((int) Math.floor(origin[0] + b[0] * sx),
                (int) Math.floor(origin[1] + b[1] * sy), (int) Math.ceil(origin[0] + b[2] * sx),
                (int) Math.ceil(origin[1] + b[3] * sy));
    }

    private static AccessibilityAction action(String name) {
        switch (name) {
            case "click":
                return AccessibilityAction.ACTION_CLICK;
            case "focus":
                return AccessibilityAction.ACTION_FOCUS;
            case "scroll_forward":
                return AccessibilityAction.ACTION_SCROLL_FORWARD;
            case "scroll_backward":
                return AccessibilityAction.ACTION_SCROLL_BACKWARD;
            case "scroll_up":
                return AccessibilityAction.ACTION_SCROLL_UP;
            case "scroll_down":
                return AccessibilityAction.ACTION_SCROLL_DOWN;
            case "scroll_left":
                return AccessibilityAction.ACTION_SCROLL_LEFT;
            case "scroll_right":
                return AccessibilityAction.ACTION_SCROLL_RIGHT;
            case "set_progress":
                return AccessibilityAction.ACTION_SET_PROGRESS;
            case "set_text":
                return AccessibilityAction.ACTION_SET_TEXT;
            case "show_on_screen":
                return AccessibilityAction.ACTION_SHOW_ON_SCREEN;
            default:
                return null;
        }
    }

    private final class Provider extends AccessibilityNodeProvider {
        @Override
        public AccessibilityNodeInfo createAccessibilityNodeInfo(int virtualViewId) {
            if (host == null) {
                return null;
            }
            synchronized (LOCK) {
                if (virtualViewId == View.NO_ID) {
                    final AccessibilityNodeInfo info = new AccessibilityNodeInfo(host);
                    host.onInitializeAccessibilityNodeInfo(info);
                    final Element window = elements.get(WINDOW);
                    if (window != null) {
                        for (String child : window.children) {
                            final Element element = elements.get(child);
                            if (element != null) {
                                info.addChild(host, element.virtualId);
                            }
                        }
                    }
                    return info;
                }
                final String id = nodes.get(virtualViewId);
                final Element element = id != null ? elements.get(id) : null;
                return element != null ? info(element) : null;
            }
        }

        private AccessibilityNodeInfo info(Element element) {
            final JSONObject fields = element.fields;
            final AccessibilityNodeInfo info = new AccessibilityNodeInfo(host, element.virtualId);
            info.setPackageName(activity.getPackageName());
            info.setSource(host, element.virtualId);
            final Element parent = elements.get(element.parent);
            if (parent == null || parent.virtualId == View.NO_ID) {
                info.setParent(host);
            } else {
                info.setParent(host, parent.virtualId);
            }
            info.setBoundsInScreen(screenBounds(element));
            info.setClassName(fields.optString("class_name", "android.view.View"));
            final String text = fields.optString("text", "");
            if (!text.isEmpty()) {
                info.setText(text);
            }
            final String hint = fields.optString("hint", "");
            if (!hint.isEmpty()) {
                info.setHintText(hint);
            }
            final String tooltip = fields.optString("tooltip", "");
            if (!tooltip.isEmpty()) {
                info.setTooltipText(tooltip);
            }
            final String state = fields.optString("state", "");
            if (!state.isEmpty()) {
                info.setStateDescription(state);
            }
            info.setCheckable(fields.optBoolean("checkable", false));
            info.setChecked(fields.optBoolean("checked", false));
            info.setEnabled(fields.optBoolean("enabled", true));
            info.setFocusable(fields.optBoolean("focusable", false));
            info.setFocused(element == elements.get(focus));
            info.setAccessibilityFocused(element.virtualId == accessibilityFocus);
            info.setEditable(fields.optBoolean("editable", false));
            info.setMultiLine(fields.optBoolean("multi_line", false));
            info.setScrollable(fields.optBoolean("scrollable", false));
            info.setSelected(fields.optBoolean("selected", false));
            info.setVisibleToUser(fields.optBoolean("visible", true));
            info.setLiveRegion(fields.optInt("live_region", 0));
            final JSONArray range = fields.optJSONArray("range");
            if (range != null && range.length() == 3) {
                info.setRangeInfo(new RangeInfo(RangeInfo.RANGE_TYPE_FLOAT,
                        (float) range.optDouble(1), (float) range.optDouble(2),
                        (float) range.optDouble(0)));
            }
            for (String child : element.children) {
                final Element inside = elements.get(child);
                if (inside != null) {
                    info.addChild(host, inside.virtualId);
                }
            }
            final JSONArray actions = fields.optJSONArray("actions");
            for (int i = 0; actions != null && i < actions.length(); i++) {
                final AccessibilityAction action = action(actions.optString(i));
                if (action != null) {
                    info.addAction(action);
                }
            }
            info.addAction(element.virtualId == accessibilityFocus
                    ? AccessibilityAction.ACTION_CLEAR_ACCESSIBILITY_FOCUS
                    : AccessibilityAction.ACTION_ACCESSIBILITY_FOCUS);
            return info;
        }

        @Override
        public boolean performAction(int virtualViewId, int action, Bundle arguments) {
            if (host == null) {
                return false;
            }
            if (virtualViewId == View.NO_ID) {
                return host.performAccessibilityAction(action, arguments);
            }
            // TalkBack's cursor is the bridge's own; the game's focus moves by ACTION_FOCUS.
            if (action == AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS) {
                if (accessibilityFocus == virtualViewId) {
                    return false;
                }
                final int previous = accessibilityFocus;
                accessibilityFocus = virtualViewId;
                if (previous != View.NO_ID) {
                    send(event(previous, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED));
                }
                host.invalidate();
                send(event(virtualViewId, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED));
                return true;
            }
            if (action == AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS) {
                if (accessibilityFocus != virtualViewId) {
                    return false;
                }
                accessibilityFocus = View.NO_ID;
                host.invalidate();
                send(event(virtualViewId,
                        AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED));
                return true;
            }
            synchronized (LOCK) {
                final String id = nodes.get(virtualViewId);
                final Element element = id != null ? elements.get(id) : null;
                final JSONArray actions =
                        element != null ? element.fields.optJSONArray("actions") : null;
                for (int i = 0; actions != null && i < actions.length(); i++) {
                    final String name = actions.optString(i);
                    final AccessibilityAction known = action(name);
                    if (known == null || known.getId() != action) {
                        continue;
                    }
                    try {
                        final JSONObject request = new JSONObject();
                        request.put("node", id);
                        request.put("action", name);
                        if ("set_progress".equals(name) && arguments != null) {
                            request.put("value", arguments.getFloat(
                                    AccessibilityNodeInfo.ACTION_ARGUMENT_PROGRESS_VALUE));
                        } else if ("set_text".equals(name) && arguments != null) {
                            final CharSequence value = arguments.getCharSequence(
                                    AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE);
                            request.put("text", value != null ? value.toString() : "");
                        }
                        queue.put(request);
                    } catch (JSONException error) {
                        return false;
                    }
                    return true;
                }
            }
            return false;
        }

        @Override
        public AccessibilityNodeInfo findFocus(int focusType) {
            if (focusType == AccessibilityNodeInfo.FOCUS_ACCESSIBILITY) {
                return accessibilityFocus != View.NO_ID
                        ? createAccessibilityNodeInfo(accessibilityFocus)
                        : null;
            }
            synchronized (LOCK) {
                final Element element = elements.get(focus);
                return element != null && element.virtualId != View.NO_ID
                        ? createAccessibilityNodeInfo(element.virtualId)
                        : null;
            }
        }
    }
}
