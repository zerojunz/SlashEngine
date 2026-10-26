#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CoinPickup.generated.h"

// A coin that spins in place and adds to the score when the player runs through it.
UCLASS()
class CRATERUN_API ACoinPickup : public AActor
{
	GENERATED_BODY()

public:
	ACoinPickup();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Coin")
	int32 Value = 1;

	UPROPERTY(EditAnywhere, Category = "Coin")
	float SpinSpeed = 90.f;

	UFUNCTION(BlueprintCallable, Category = "Coin")
	void Collect(class ACharacter* Collector);

protected:
	virtual void Tick(float DeltaSeconds) override;

	UPROPERTY(VisibleAnywhere)
	class USphereComponent* Trigger;
};
