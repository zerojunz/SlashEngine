package org.slashengine.billing;

import android.app.Activity;
import com.android.billingclient.api.AcknowledgePurchaseParams;
import com.android.billingclient.api.BillingClient;
import com.android.billingclient.api.BillingClientStateListener;
import com.android.billingclient.api.BillingFlowParams;
import com.android.billingclient.api.BillingResult;
import com.android.billingclient.api.ConsumeParams;
import com.android.billingclient.api.PendingPurchasesParams;
import com.android.billingclient.api.ProductDetails;
import com.android.billingclient.api.Purchase;
import com.android.billingclient.api.PurchasesUpdatedListener;
import com.android.billingclient.api.QueryProductDetailsParams;
import com.android.billingclient.api.QueryProductDetailsResult;
import com.android.billingclient.api.QueryPurchasesParams;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import org.slashengine.services.PurchasesBackend;
import org.slashengine.services.Reports;

/**
 * The purchases backend over Google Play Billing, as Godot's Google Play Billing plugin uses it:
 * product details for the products, the billing flow for purchases, consuming consumables and
 * acknowledging the rest when the game finishes them, and the purchases Play holds for restore.
 * Transactions and receipts are Play's purchase tokens, which the game's server checks with the
 * Google Play Developer API. Everything runs on the UI thread, where Play calls back.
 */
public final class PlayBillingPurchases implements PurchasesBackend, PurchasesUpdatedListener {
    private Activity activity;
    private Reports reports;
    private BillingClient client;
    // What runs once the client is connected, and what runs if it can't connect.
    private final List<Runnable[]> waiting = new ArrayList<>();
    private boolean connecting;
    // Why the client couldn't connect, the last time it didn't.
    private String lastError = "";
    // The game's products: their types, and what Play said about each, by ID.
    private final Map<String, String> types = new HashMap<>();
    private final Map<String, ProductDetails> details = new HashMap<>();
    // Purchases the game hasn't finished, by purchase token.
    private final Map<String, Purchase> purchases = new HashMap<>();
    // The product of the billing flow on screen, for a cancel that names none.
    private String buying;

    @Override
    public String name() {
        return "google_play";
    }

    @Override
    public void start(Activity activity, Reports reports) {
        this.activity = activity;
        this.reports = reports;
        activity.runOnUiThread(
                () ->
                        client =
                                BillingClient.newBuilder(activity)
                                        .setListener(this)
                                        .enablePendingPurchases(
                                                PendingPurchasesParams.newBuilder()
                                                        .enableOneTimeProducts()
                                                        .build())
                                        .build());
    }

    /**
     * Runs work on the UI thread once the client is connected, connecting it if it isn't, or
     * failed with why it couldn't connect (lastError).
     */
    private void whenReady(Runnable work, Runnable failed) {
        activity.runOnUiThread(
                () -> {
                    if (client.isReady()) {
                        work.run();
                        return;
                    }
                    waiting.add(new Runnable[] {work, failed});
                    if (connecting) {
                        return;
                    }
                    connecting = true;
                    client.startConnection(
                            new BillingClientStateListener() {
                                @Override
                                public void onBillingSetupFinished(BillingResult result) {
                                    activity.runOnUiThread(() -> connected(result));
                                }

                                @Override
                                public void onBillingServiceDisconnected() {
                                    // The next call connects again.
                                }
                            });
                });
    }

    private void connected(BillingResult result) {
        connecting = false;
        boolean ok = result.getResponseCode() == BillingClient.BillingResponseCode.OK;
        if (!ok) {
            lastError = describe(result);
        }
        List<Runnable[]> ready = new ArrayList<>(waiting);
        waiting.clear();
        for (Runnable[] each : ready) {
            each[ok ? 0 : 1].run();
        }
    }

    private static String describe(BillingResult result) {
        String message = result.getDebugMessage();
        return message == null || message.isEmpty()
                ? "Google Play Billing answered " + result.getResponseCode()
                : message;
    }

    private static String productType(String type) {
        return "subscription".equals(type)
                ? BillingClient.ProductType.SUBS
                : BillingClient.ProductType.INAPP;
    }

    @Override
    public void fetchProducts(String catalogJson) {
        List<QueryProductDetailsParams.Product> inapp = new ArrayList<>();
        List<QueryProductDetailsParams.Product> subs = new ArrayList<>();
        try {
            JSONArray catalog = new JSONArray(catalogJson);
            for (int i = 0; i < catalog.length(); ++i) {
                JSONObject product = catalog.getJSONObject(i);
                String id = product.getString("id");
                String type = product.getString("type");
                types.put(id, type);
                QueryProductDetailsParams.Product query =
                        QueryProductDetailsParams.Product.newBuilder()
                                .setProductId(id)
                                .setProductType(productType(type))
                                .build();
                ("subscription".equals(type) ? subs : inapp).add(query);
            }
        } catch (JSONException error) {
            reports.productsFailed("the catalog isn't valid: " + error.getMessage());
            return;
        }
        whenReady(
                () -> {
                    // Play takes one product type a query.
                    List<ProductDetails> found = new ArrayList<>();
                    int[] left = {(inapp.isEmpty() ? 0 : 1) + (subs.isEmpty() ? 0 : 1)};
                    String[] failure = {null};
                    Runnable done =
                            () -> {
                                if (--left[0] > 0) {
                                    return;
                                }
                                if (failure[0] != null) {
                                    reports.productsFailed(failure[0]);
                                } else {
                                    reports.products(productsJson(found));
                                }
                                deliverUnfinished();
                            };
                    if (left[0] == 0) {
                        ++left[0];
                        done.run();
                        return;
                    }
                    for (List<QueryProductDetailsParams.Product> list :
                            Arrays.asList(inapp, subs)) {
                        if (list.isEmpty()) {
                            continue;
                        }
                        client.queryProductDetailsAsync(
                                QueryProductDetailsParams.newBuilder()
                                        .setProductList(list)
                                        .build(),
                                (BillingResult result, QueryProductDetailsResult answer) ->
                                        activity.runOnUiThread(
                                                () -> {
                                                    if (result.getResponseCode()
                                                            == BillingClient.BillingResponseCode
                                                                    .OK) {
                                                        found.addAll(
                                                                answer.getProductDetailsList());
                                                    } else {
                                                        failure[0] = describe(result);
                                                    }
                                                    done.run();
                                                }));
                    }
                },
                () -> reports.productsFailed(lastError));
    }

    /** What Play said about the products, as Reports.products takes it. */
    private String productsJson(List<ProductDetails> found) {
        JSONArray out = new JSONArray();
        try {
            for (ProductDetails product : found) {
                details.put(product.getProductId(), product);
                JSONObject item = new JSONObject();
                item.put("id", product.getProductId());
                item.put("title", product.getName());
                item.put("description", product.getDescription());
                ProductDetails.OneTimePurchaseOfferDetails once =
                        product.getOneTimePurchaseOfferDetails();
                ProductDetails.PricingPhase phase = recurringPhase(product);
                if (once != null) {
                    item.put("price", once.getFormattedPrice());
                    item.put("price_micros", once.getPriceAmountMicros());
                    item.put("currency", once.getPriceCurrencyCode());
                } else if (phase != null) {
                    item.put("price", phase.getFormattedPrice());
                    item.put("price_micros", phase.getPriceAmountMicros());
                    item.put("currency", phase.getPriceCurrencyCode());
                }
                out.put(item);
            }
            for (String id : types.keySet()) {
                if (!details.containsKey(id)) {
                    out.put(new JSONObject().put("id", id).put("available", false));
                }
            }
        } catch (JSONException error) {
            // put only throws for numbers that aren't finite.
        }
        return out.toString();
    }

    /** A subscription's base plan, the offer with no offer ID, or else its first offer. */
    private static ProductDetails.SubscriptionOfferDetails basePlan(ProductDetails product) {
        List<ProductDetails.SubscriptionOfferDetails> offers =
                product.getSubscriptionOfferDetails();
        if (offers == null || offers.isEmpty()) {
            return null;
        }
        for (ProductDetails.SubscriptionOfferDetails offer : offers) {
            if (offer.getOfferId() == null) {
                return offer;
            }
        }
        return offers.get(0);
    }

    /** The price a subscription's base plan renews at: its last pricing phase. */
    private static ProductDetails.PricingPhase recurringPhase(ProductDetails product) {
        ProductDetails.SubscriptionOfferDetails plan = basePlan(product);
        if (plan == null) {
            return null;
        }
        List<ProductDetails.PricingPhase> phases = plan.getPricingPhases().getPricingPhaseList();
        return phases.isEmpty() ? null : phases.get(phases.size() - 1);
    }

    /** Reports the purchases Play holds that the game hasn't finished. */
    private void deliverUnfinished() {
        queryPurchases(
                (purchase, type) -> {
                    if (purchase.getPurchaseState() == Purchase.PurchaseState.PENDING) {
                        report(purchase, "pending");
                    } else if (purchase.getPurchaseState() == Purchase.PurchaseState.PURCHASED
                            && ("consumable".equals(type) || !purchase.isAcknowledged())) {
                        report(purchase, "purchased");
                    }
                },
                null);
    }

    private interface PurchaseVisitor {
        void visit(Purchase purchase, String type);
    }

    /** Visits what Play holds of both product types, then runs done with an error or null. */
    private void queryPurchases(PurchaseVisitor visitor, ErrorCallback done) {
        String[] queries = {BillingClient.ProductType.INAPP, BillingClient.ProductType.SUBS};
        int[] left = {queries.length};
        String[] failure = {null};
        for (String query : queries) {
            client.queryPurchasesAsync(
                    QueryPurchasesParams.newBuilder().setProductType(query).build(),
                    (BillingResult result, List<Purchase> list) ->
                            activity.runOnUiThread(
                                    () -> {
                                        if (result.getResponseCode()
                                                == BillingClient.BillingResponseCode.OK) {
                                            for (Purchase purchase : list) {
                                                visitor.visit(purchase, typeOf(purchase, query));
                                            }
                                        } else {
                                            failure[0] = describe(result);
                                        }
                                        if (--left[0] == 0 && done != null) {
                                            done.run(failure[0]);
                                        }
                                    }));
        }
    }

    private interface ErrorCallback {
        void run(String error);
    }

    private String typeOf(Purchase purchase, String query) {
        List<String> products = purchase.getProducts();
        String type = products.isEmpty() ? null : types.get(products.get(0));
        if (type != null) {
            return type;
        }
        return BillingClient.ProductType.SUBS.equals(query) ? "subscription" : "non_consumable";
    }

    private void report(Purchase purchase, String state) {
        String token = purchase.getPurchaseToken();
        if (!"pending".equals(state)) {
            purchases.put(token, purchase);
        }
        for (String product : purchase.getProducts()) {
            reports.purchase(product, token, state, token, null);
        }
    }

    @Override
    public void purchase(String product, String type) {
        whenReady(
                () -> {
                    ProductDetails found = details.get(product);
                    if (found == null) {
                        reports.purchase(
                                product,
                                null,
                                "failed",
                                null,
                                "Google Play didn't list this product");
                        return;
                    }
                    BillingFlowParams.ProductDetailsParams.Builder params =
                            BillingFlowParams.ProductDetailsParams.newBuilder()
                                    .setProductDetails(found);
                    ProductDetails.SubscriptionOfferDetails plan = basePlan(found);
                    if (plan != null) {
                        params.setOfferToken(plan.getOfferToken());
                    }
                    buying = product;
                    BillingResult result =
                            client.launchBillingFlow(
                                    activity,
                                    BillingFlowParams.newBuilder()
                                            .setProductDetailsParamsList(
                                                    Collections.singletonList(params.build()))
                                            .build());
                    if (result.getResponseCode() != BillingClient.BillingResponseCode.OK) {
                        buying = null;
                        reports.purchase(product, null, "failed", null, describe(result));
                    }
                },
                () -> reports.purchase(product, null, "failed", null, lastError));
    }

    @Override
    public void onPurchasesUpdated(BillingResult result, List<Purchase> updated) {
        activity.runOnUiThread(
                () -> {
                    String product = buying;
                    buying = null;
                    int code = result.getResponseCode();
                    if (code == BillingClient.BillingResponseCode.OK && updated != null) {
                        for (Purchase purchase : updated) {
                            report(
                                    purchase,
                                    purchase.getPurchaseState() == Purchase.PurchaseState.PENDING
                                            ? "pending"
                                            : "purchased");
                        }
                    } else if (product != null) {
                        boolean cancelled = code == BillingClient.BillingResponseCode.USER_CANCELED;
                        String error =
                                code == BillingClient.BillingResponseCode.ITEM_ALREADY_OWNED
                                        ? "already owned; restore purchases to get it back"
                                        : describe(result);
                        reports.purchase(
                                product,
                                null,
                                cancelled ? "cancelled" : "failed",
                                null,
                                cancelled ? null : error);
                    }
                });
    }

    @Override
    public void finish(String product, String type, String transaction, String receipt) {
        whenReady(
                () -> {
                    Purchase purchase = purchases.remove(transaction);
                    if ("consumable".equals(type)) {
                        client.consumeAsync(
                                ConsumeParams.newBuilder().setPurchaseToken(transaction).build(),
                                (BillingResult result, String token) -> {});
                    } else if (purchase == null || !purchase.isAcknowledged()) {
                        client.acknowledgePurchase(
                                AcknowledgePurchaseParams.newBuilder()
                                        .setPurchaseToken(transaction)
                                        .build(),
                                (BillingResult result) -> {});
                    }
                },
                () -> {});
    }

    @Override
    public void restore() {
        whenReady(
                () ->
                        queryPurchases(
                                (purchase, type) -> {
                                    if (purchase.getPurchaseState()
                                                    == Purchase.PurchaseState.PURCHASED
                                            && !"consumable".equals(type)) {
                                        report(purchase, "restored");
                                    }
                                },
                                error -> {
                                    if (error == null) {
                                        reports.restoreFinished();
                                    } else {
                                        reports.restoreFinished(error);
                                    }
                                }),
                () -> reports.restoreFinished(lastError));
    }
}
