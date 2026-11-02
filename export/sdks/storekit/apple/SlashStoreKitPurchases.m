// The purchases backend over Apple's StoreKit, as Godot's iOS in-app store plugin uses it:
// SKProductsRequest for the products, SKPaymentQueue for purchases and restore, and the App
// Store receipt for the game's server to check. StoreKit 1 is the API Objective-C can call on
// macOS 11 and iOS 14; StoreKit 2 is Swift only. Built with ARC.

#import "slash/services/apple.h"

#import <StoreKit/StoreKit.h>

// StoreKit 1 is deprecated from iOS 18 and macOS 15 on, but it is what reaches back to the
// systems games support.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

@interface SlashStoreKitPurchases
    : NSObject <SlashPurchasesBackend, SKProductsRequestDelegate, SKPaymentTransactionObserver>
@end

@implementation SlashStoreKitPurchases {
    SlashServiceReports* _reports;
    SKProductsRequest* _request;
    // What the store said about each product, by its ID.
    NSMutableDictionary<NSString*, SKProduct*>* _products;
    // Transactions the game hasn't finished, by their identifiers.
    NSMutableDictionary<NSString*, SKPaymentTransaction*>* _transactions;
    BOOL _observing;
}

- (NSString*)name {
    return @"storekit";
}

- (void)startWithReports:(SlashServiceReports*)reports {
    _reports = reports;
    _products = [NSMutableDictionary dictionary];
    _transactions = [NSMutableDictionary dictionary];
}

- (void)dealloc {
    if (_observing) {
        [[SKPaymentQueue defaultQueue] removeTransactionObserver:self];
    }
}

// StoreKit delivers the transactions it holds once an observer is added, so purchases the game
// didn't finish come after the products, as the engine expects.
- (void)observe {
    if (!_observing) {
        _observing = YES;
        [[SKPaymentQueue defaultQueue] addTransactionObserver:self];
    }
}

- (void)fetchProducts:(NSArray<NSDictionary<NSString*, NSString*>*>*)catalog {
    NSMutableSet<NSString*>* ids = [NSMutableSet set];
    for (NSDictionary<NSString*, NSString*>* product in catalog) {
        [ids addObject:product[@"id"]];
    }
    [_request cancel];
    _request = [[SKProductsRequest alloc] initWithProductIdentifiers:ids];
    _request.delegate = self;
    [_request start];
}

- (void)productsRequest:(SKProductsRequest*)request
     didReceiveResponse:(SKProductsResponse*)response {
    NSMutableArray<NSDictionary<NSString*, id>*>* products = [NSMutableArray array];
    for (SKProduct* product in response.products) {
        _products[product.productIdentifier] = product;
        NSNumberFormatter* formatter = [[NSNumberFormatter alloc] init];
        formatter.numberStyle = NSNumberFormatterCurrencyStyle;
        formatter.locale = product.priceLocale;
        NSString* currency = product.priceLocale.currencyCode;
        [products addObject:@{
            @"id" : product.productIdentifier,
            @"title" : product.localizedTitle ?: @"",
            @"description" : product.localizedDescription ?: @"",
            @"price" : [formatter stringFromNumber:product.price] ?: @"",
            @"price_micros" : @([[product.price decimalNumberByMultiplyingByPowerOf10:6]
                longLongValue]),
            @"currency" : currency ?: @"",
            @"available" : @YES,
        }];
    }
    for (NSString* missing in response.invalidProductIdentifiers) {
        [products addObject:@{@"id" : missing, @"available" : @NO}];
    }
    [_reports products:products];
    [self observe];
}

- (void)request:(SKRequest*)request didFailWithError:(NSError*)error {
    [_reports productsFailed:error.localizedDescription ?: @"the App Store didn't answer"];
    [self observe];
}

- (void)purchase:(NSString*)product type:(NSString*)type {
    SKProduct* found = _products[product];
    NSString* error = nil;
    if (![SKPaymentQueue canMakePayments]) {
        error = @"purchases are turned off on this device";
    } else if (found == nil) {
        error = @"the App Store didn't list this product";
    }
    if (error != nil) {
        [_reports purchase:product transaction:nil state:@"failed" receipt:nil error:error];
        return;
    }
    [self observe];
    [[SKPaymentQueue defaultQueue] addPayment:[SKPayment paymentWithProduct:found]];
}

- (void)finish:(NSString*)product
           type:(NSString*)type
    transaction:(NSString*)transaction
        receipt:(NSString*)receipt {
    SKPaymentTransaction* found = _transactions[transaction];
    if (found != nil) {
        [_transactions removeObjectForKey:transaction];
        [[SKPaymentQueue defaultQueue] finishTransaction:found];
    }
}

- (void)restore {
    [self observe];
    [[SKPaymentQueue defaultQueue] restoreCompletedTransactions];
}

// The app's receipt, which holds every purchase, for the game's server to check with Apple.
- (nullable NSString*)receipt {
    NSURL* url = [NSBundle mainBundle].appStoreReceiptURL;
    NSData* data = url != nil ? [NSData dataWithContentsOfURL:url] : nil;
    return [data base64EncodedStringWithOptions:0];
}

- (void)paymentQueue:(SKPaymentQueue*)queue
    updatedTransactions:(NSArray<SKPaymentTransaction*>*)transactions {
    for (SKPaymentTransaction* transaction in transactions) {
        NSString* product = transaction.payment.productIdentifier;
        NSString* identifier = transaction.transactionIdentifier;
        switch (transaction.transactionState) {
        case SKPaymentTransactionStatePurchasing:
            break;
        case SKPaymentTransactionStatePurchased:
        case SKPaymentTransactionStateRestored: {
            const BOOL restored = transaction.transactionState == SKPaymentTransactionStateRestored;
            if (identifier != nil) {
                _transactions[identifier] = transaction;
            }
            [_reports purchase:product
                   transaction:identifier
                         state:restored ? @"restored" : @"purchased"
                       receipt:[self receipt]
                         error:nil];
            break;
        }
        case SKPaymentTransactionStateDeferred:
            // Waiting for a parent's approval (Ask to Buy); the purchase comes later.
            [_reports purchase:product transaction:nil state:@"pending" receipt:nil error:nil];
            break;
        case SKPaymentTransactionStateFailed: {
            NSError* error = transaction.error;
            const BOOL cancelled = [error.domain isEqualToString:SKErrorDomain] &&
                                   error.code == SKErrorPaymentCancelled;
            [_reports purchase:product
                   transaction:identifier
                         state:cancelled ? @"cancelled" : @"failed"
                       receipt:nil
                         error:cancelled ? nil
                                         : (error.localizedDescription ?: @"the purchase failed")];
            [queue finishTransaction:transaction];
            break;
        }
        }
    }
}

- (void)paymentQueueRestoreCompletedTransactionsFinished:(SKPaymentQueue*)queue {
    [_reports restoreFinished:nil];
}

- (void)paymentQueue:(SKPaymentQueue*)queue
    restoreCompletedTransactionsFailedWithError:(NSError*)error {
    [_reports restoreFinished:error.localizedDescription ?: @"restoring purchases failed"];
}

@end
