Animator CubeGuyAnimator
	Parameter speed Float 1      # 0 = idle, 1 = full walk; a script drives it (self.animator.setFloat)

	BlendSpace1D locomotion speed
		Sample cubeguy_idle 0.0
		Sample cubeguy_walk 1.0

	StateMachine
		Entry Move
		State Move
			Play locomotion
