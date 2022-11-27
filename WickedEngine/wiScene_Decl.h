#pragma once


namespace wiScene
{
	struct NameComponent;
	struct LayerComponent;
	struct TransformComponent;
	struct PreviousFrameTransformComponent;
	struct HierarchyComponent;
	struct MaterialComponent;
	struct MeshComponent;
	struct ImpostorComponent;
	struct ObjectComponent;
	struct RigidBodyPhysicsComponent;
#ifndef GGREDUCED //PE: Remove all physics checks, we dont use it.
	struct SoftBodyPhysicsComponent;
#endif
	struct ArmatureComponent;
	struct LightComponent;
	struct CameraComponent;
	struct EnvironmentProbeComponent;
	struct ForceFieldComponent;
	struct DecalComponent;
	struct AnimationComponent;
	struct AnimationDataComponent;
	struct WeatherComponent;
	struct SoundComponent;
	struct InverseKinematicsComponent;
	struct SpringComponent;
	struct Scene;

	class wiEmittedParticle; // todo: rename
	class wiHairParticle; // todo: rename
}
