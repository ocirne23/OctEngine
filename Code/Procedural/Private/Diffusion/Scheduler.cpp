module Procedural;

import Core;
import :Diffusion.Scheduler;

// Precision note: this file is compiled /fp:precise (see Procedural/CMakeLists.txt) and the double/float
// mixing below is REPRODUCED FROM THE REFERENCE, not incidental. Java evaluates Math.log/atan/pow/sqrt in
// double and narrows on assignment; the explicit casts here pin the same widths. Do not "clean up" by
// switching to the float overloads or by making everything double — the sigma schedule feeds every
// downstream value, so a last-bit difference changes the terrain.
namespace Procedural::Diffusion
{
	namespace
	{
		constexpr float RHO = 7.0f;
	}

	oc::vector<float> EDMScheduler::computeKarrasSigmas(int32 n)
	{
		assert(n > 1); // n == 1 divides by zero below; the reference is unguarded
		const float minInvRho = (float)std::pow((double)SIGMA_MIN, 1.0 / (double)RHO);
		const float maxInvRho = (float)std::pow((double)SIGMA_MAX, 1.0 / (double)RHO);

		oc::vector<float> sigmas((size_t)n + 1);
		for (int32 i = 0; i < n; i++)
		{
			const float t = (float)i / (float)(n - 1); // float division: the cast binds to i first in Java
			const float invRho = maxInvRho + t * (minInvRho - maxInvRho);
			sigmas[(size_t)i] = (float)std::pow((double)invRho, (double)RHO);
		}
		sigmas[(size_t)n] = 0.0f; // final_sigmas_type == "zero"
		return sigmas;
	}

	EDMScheduler::EDMScheduler(int32 numSteps)
		: m_sigmas(computeKarrasSigmas(numSteps)), m_numSteps(numSteps)
	{
		reset();
	}

	void EDMScheduler::reset()
	{
		m_stepIndex = 0;
		m_lowerOrderNums = 0;
		m_prevModelOutput.clear();
	}

	void EDMScheduler::preconditionInputs(oc::span<const float> sample, float sigma, oc::span<float> out)
	{
		assert(out.size() == sample.size());
		const float arg = sigma * sigma + SIGMA_DATA * SIGMA_DATA;
		const float cIn = 1.0f / (float)std::sqrt((double)arg);
		for (size_t i = 0; i < sample.size(); i++)
			out[i] = sample[i] * cIn;
	}

	float EDMScheduler::trigflowPreconditionNoise(float sigma)
	{
		return (float)std::atan((double)(sigma / SIGMA_DATA));
	}

	void EDMScheduler::preconditionOutputs(oc::span<const float> sample, oc::span<const float> modelOut,
	                                       float sigma, oc::span<float> x0Out)
	{
		const float sd2 = SIGMA_DATA * SIGMA_DATA;
		const float sig2 = sigma * sigma;
		const float cSkip = sd2 / (sig2 + sd2);
		const float cOut = sigma * SIGMA_DATA / (float)std::sqrt((double)(sig2 + sd2));
		for (size_t i = 0; i < sample.size(); i++)
			x0Out[i] = cSkip * sample[i] + cOut * modelOut[i];
	}

	// alpha == 1 and lambda == -log(sigma) (no VP conversion), so exp(-h) collapses to sigma_t/sigma_s.
	// At the last step sigma_t == 0 => ratio == 0 => xt == x0Pred exactly.
	void EDMScheduler::firstOrderUpdate(oc::span<const float> x0Pred, oc::span<float> sample,
	                                    float sigmaS, float sigmaT)
	{
		const float ratio = sigmaT / sigmaS;
		for (size_t i = 0; i < sample.size(); i++)
			sample[i] = ratio * sample[i] - (ratio - 1.0f) * x0Pred[i];
	}

	void EDMScheduler::secondOrderUpdate(oc::span<const float> m1, oc::span<const float> m0,
	                                     oc::span<float> sample, float sigmaS0, float sigmaT, float sigmaS1)
	{
		// Logs in double, but exp(-h) is deliberately recovered as the FLOAT ratio sigma_t/sigma_s0 rather
		// than std::exp(-h) — that is what the reference does ("in float32 like Python"). The mix is intentional.
		const double lT = -std::log((double)sigmaT);
		const double lS0 = -std::log((double)sigmaS0);
		const double lS1 = -std::log((double)sigmaS1);
		const double h = lT - lS0;
		const double h0 = lS0 - lS1;
		const float r0 = (float)(h0 / h);

		const float expNH = sigmaT / sigmaS0;
		const float sCoeff = sigmaT / sigmaS0;
		const float d0Coeff = -(expNH - 1.0f);
		const float d1Coeff = -0.5f * (expNH - 1.0f);

		for (size_t i = 0; i < sample.size(); i++)
		{
			const float d1 = (m0[i] - m1[i]) / r0;
			sample[i] = sCoeff * sample[i] + d0Coeff * m0[i] + d1Coeff * d1;
		}
	}

	void EDMScheduler::step(oc::span<const float> modelOut, oc::span<float> sample)
	{
		assert(m_stepIndex < m_numSteps);
		assert(modelOut.size() == sample.size());

		const float sigmaS = m_sigmas[(size_t)m_stepIndex];
		const float sigmaT = m_sigmas[(size_t)m_stepIndex + 1];

		m_x0Pred.resize(sample.size());
		preconditionOutputs(sample, modelOut, sigmaS, m_x0Pred);

		const bool lowerOrderFinal = (m_stepIndex == m_numSteps - 1);
		if (m_lowerOrderNums < 1 || lowerOrderFinal)
		{
			firstOrderUpdate(m_x0Pred, sample, sigmaS, sigmaT);
		}
		else
		{
			assert(!m_prevModelOutput.empty());
			secondOrderUpdate(m_prevModelOutput, m_x0Pred, sample, sigmaS, sigmaT, m_sigmas[(size_t)m_stepIndex - 1]);
		}

		// prevModelOutput = x0Pred. Swap rather than copy; m_x0Pred is scratch and is refilled next step.
		m_prevModelOutput.swap(m_x0Pred);
		if (m_lowerOrderNums < 2)
			m_lowerOrderNums++;
		m_stepIndex++;
	}
}
