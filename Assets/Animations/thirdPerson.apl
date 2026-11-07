# The local key C capsule (playerCapsule.pre). App.PlayerLocomotion drives both parameters each frame:
# gait 0 idle .. 1 walk .. 2 run, and rate = the stride-matched playback rate (negative = walking backward).
Animator ThirdPersonAnimator
	Parameter gait Float 0
	Parameter rate Float 1

	Clip idle Anim idle
	Clip walk Anim walk
	Clip run Anim run

	BlendSpace1D locomotion gait
		Sample idle 0.0
		Sample walk 1.0
		Sample run 2.0

	StateMachine
		Entry Move
		State Move
			Play locomotion
			SpeedParam rate
			SpeedScale 1.0
