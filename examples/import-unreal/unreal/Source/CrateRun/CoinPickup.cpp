#include "CoinPickup.h"

#include "Components/SphereComponent.h"
#include "GameFramework/Character.h"
#include "Kismet/GameplayStatics.h"

ACoinPickup::ACoinPickup()
{
	PrimaryActorTick.bCanEverTick = true;
	Trigger = CreateDefaultSubobject<USphereComponent>(TEXT("Trigger"));
	RootComponent = Trigger;
}

void ACoinPickup::Collect(ACharacter* Collector)
{
	UGameplayStatics::PlaySoundAtLocation(this, nullptr, GetActorLocation());
	Destroy();
}

void ACoinPickup::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	AddActorLocalRotation(FRotator(0.f, SpinSpeed * DeltaSeconds, 0.f));
}
