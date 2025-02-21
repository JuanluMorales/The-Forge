/*
 * Copyright (c) 2018-2021 Confetti Interactive Inc.
 *
 * This is a part of Aura.
 *
 * This file(code) is licensed under a
 * Creative Commons Attribution-NonCommercial 4.0 International License
 *
 *   (https://creativecommons.org/licenses/by-nc/4.0/legalcode)
 *
 * Based on a work at https://github.com/ConfettiFX/The-Forge.
 * You may not use the material for commercial purposes.
*/

#ifndef LPV_LIGHT_APPLY_H
#define LPV_LIGHT_APPLY_H

#include "lpvCommon_example.h"
#include "lpvSHMaths_example.h"

STRUCT(AuraIndirectLight) 
{
	DATA(float4, diffuse,  None);
	DATA(float4, specular, None);
};

/*
	Applies indirect lighting from the cascaded light propagation volumes.

	clipSpacePosition: The clip-space position of the current pixel being shades. i.e float3(textureCoordinate.xy, depth).

*/
void Aura_ApplyLightForCascade(
                               const uint cascadeIndex,
                               const float3 worldSpacePosition,
                               const float3 normal,
                               float3 normalScale,
                               float3 WorldToGridScale,
                               float3 WorldToGridTranslate,
                               float3 smoothGridPosOffset,
                               float3 cameraPosition,
                               float lightScale,
                               float cascadeScale,
                               float fresnelScale,
                               float specularScale,
                               float specularPower,
                               float4 cellFalloff,
                               const int specularSteps,
                               SamplerState auraSampler,
                               inout(AuraIndirectLight) indirectLight)
{
	//float3 gridPos0 = mul(WorldToGrid, float4(pos,1)).xyz;
	float3 gradNScale = normalScale;

	float3 gridPos0 = worldSpacePosition * WorldToGridScale + WorldToGridTranslate;
	float3 gridPos1 = gridPos0 + normal * gradNScale;

	float weight = distance(gridPos0, gridPos1);
	//float weight = dot(gridPos0,gridPos1) - dot(gridPos1,gridPos0);

	float borderFactor = calculateBorderFadeout(gridPos0 + smoothGridPosOffset);

	//	Igor: This can be done using depth-test. Need to copy depth-buffer or
	//	use dx11.
	//	Early out
	//	TODO: Igor: use depth test here for dx11 class hardware!
	if (borderFactor <= 0.0f) 
		return;

	// tim: hack to stop some light bleeding
	//borderFactor = lerp(0.0, borderFactor,saturate(dot(-lightDir, normal)));

	SHSpectralCoeffs spCoeffs0 = SHSampleGridForCascade(cascadeIndex, auraSampler, gridPos0, i3(0));


	//////////////////////
	//	Gradient detection	
	SHSpectralCoeffs spCoeffs1 = SHSampleGridForCascade(cascadeIndex, auraSampler, gridPos1, i3(0));

	/*
	float3 dumpFactor = (0).xxx;
	{	//	Hide variable i in local scope
	[unroll]
	for (int i=0; i<3; ++i)
	{
	float4 delta = spCoeffs0.c[i]-spCoeffs1.c[i];


	//	This seems to work better than just comparing primary light propagation directions
	//	It seems that for the current setup intensity only chekc is the best.

	//[flatten]
	//if (dot(spCoeffs0.c[i].yzw,spCoeffs0.c[i].yzw)>0.000001)
	//if (dot(spCoeffs0.c[i].yzw,spCoeffs0.c[i].yzw)>0.00000001)
	//    dumpFactor[i] = smoothstep(-0.3, -1, dot(-delta.yzw,spCoeffs0.c[i].yzw)/dot(spCoeffs0.c[i].yzw,spCoeffs0.c[i].yzw));
	float denom = max(dot(spCoeffs0.c[i].yzw,spCoeffs0.c[i].yzw), 0.000001);
	dumpFactor[i] = smoothstep(-0.3, -1, dot(-delta.yzw,spCoeffs0.c[i].yzw)/denom);

	//  Only constant term comparison seems to work pretty ok
	//[flatten]
	//if (spCoeffs0.c[i].x>0.000001)
	//if (spCoeffs0.c[i].x>0.00000001)
	//    dumpFactor[i] = saturate(dumpFactor[i]*4*smoothstep( -0.1, 1, delta.x/spCoeffs0.c[i].x));
	denom = spCoeffs0.c[i].x;
	dumpFactor[i] = saturate(dumpFactor[i]*4*smoothstep( -0.1, 1, delta.x/denom));

	//dumpFactor[i] = saturate(dumpFactor[i]*4*smoothstep( 0.001, 0.1, delta.x));
	}
	}
	float weight = max(max(dumpFactor[0],dumpFactor[1]),dumpFactor[2]);
	*/


	//-->lttv - better with max blending
	//-->spCoeffs0 = SHLerp(spCoeffs0, spCoeffs1, dumpFactor[0]);
	spCoeffs0 = SHLerp(spCoeffs0, spCoeffs1, weight);
	//<--lttv

	//	Offset grid position to allow gradient-fixed specular sampling
	//-->lttv
	gridPos0 += normal * gradNScale * weight;
	//-->gridPos0 += normal*gradNScale*rrr;
	//<--

	//	Gradient detection
	//////////////////////

	////////////////////////
	//	Diffuse
	SHCoeffs shCosineLobe = SHProjectCone(-normal);
	//SHCoeffs shCosineLobe = SHProjectCone(abs(normal));

	// integrate incoming radiance with cosine lobe function 
	float3 incidentLuminance = max(f3(0.0f), SHDot(spCoeffs0, shCosineLobe));
	//incidentLuminance *= cellFalloff.x;

	//	Diffuse
	////////////////////////

	///////////////////////////
	//	Specular

	float3 camToPoint = normalize(worldSpacePosition - cameraPosition);
	float3 reflected = reflect(camToPoint, normal);

	SHSpectralCoeffs spCoeffsSpec[4];
	spCoeffsSpec[0] = spCoeffs0;
	{
		// Removed unroll as it causes problems with dxc
		// [unroll]
		for (int i = 1; i < specularSteps; ++i)
			spCoeffsSpec[i] = SHSampleGridForCascade(cascadeIndex, auraSampler, gridPos0 + (reflected * normalScale) * i, i3(0));
	}

	float specScalarFactor = 1.0f;
	float3 specular = f3(0.0f);
	{
		//	Add fresnel reflection term

		//const float specNormFactor[] = { 1.0f, 0.451389f, 0.3125f, 0.266393f, 0.25f };

		// Removed unroll as it causes problems with dxc
		// [unroll]
		for (int i = 0; i < specularSteps; ++i)
		{
			float3 specSH = max(SHEvaluateFunction(-reflected, spCoeffsSpec[i]), f3(0.0f));

			// Fresnel moved into specular -> more steps more fresnel
			float NdotL = max(0.0f, dot(-camToPoint, -reflected));
			float fresnel = pow(NdotL, specularPower);
			fresnel += fresnelScale * (1.0f - fresnel);
			specScalarFactor = fresnel;
			//specScalarFactor *= specNormFactor[SPECULAR_STEPS];
			specular += specSH * specScalarFactor * specularScale;
			//specular *= *cellFalloff[i];
		}
	}

	//	Specular
	///////////////////////////

	//-->lttv better propagation

	//res.specular.rgb = (specular*normalNSpec.a) * (lightScale*lumScale.x);
	float borderFactorAndFalloff = saturate(borderFactor * cellFalloff.z);

	indirectLight.specular.rgb += borderFactorAndFalloff * (specular) * (lightScale * cascadeScale);
	// indirectLight.specular.a = 

	indirectLight.diffuse.rgb += borderFactorAndFalloff * incidentLuminance * (lightScale * cascadeScale);
	// indirectLight.diffuse.a = saturate(borderFactor * cellFalloff.z);
	//--> #else
	/*
	res.specular.rgb = specular*lightScale*specScalarFactor;
	res.specular.a = 1.0;

	res.diffuse.rgb = incidentLuminance*lightScale*cellFalloff.x*lumScale.x*borderFactor;
	res.diffuse.a = 1.0;
	*/
	//<-- #endif
}

#endif // LPV_LIGHT_APPLY_H
