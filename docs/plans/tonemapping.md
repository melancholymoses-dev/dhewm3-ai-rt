# Doom 3 RT Tonemapping and Volumetrics Plan

This document outlines the strategy for resolving washed-out visuals caused by adding Global Illumination (GI) and fixing Volumetric "blobs" in the Doom 3 Vulkan Ray Tracing renderer.

## 1. Tonemapping and the "Toe" (Fixing the Washed-Out Look)

Adding GI to Doom 3 often washes out the harsh, high-contrast look by filling in pitch-black stencil shadows. To fix this without breaking the linear color ratios (hue), we must apply tonemapping to the *final combined lighting* (Direct + GI + Volumetric). 

### Solution: Uchimura Tonemap
Implement a parameterized Filmic curve (Uchimura, used in Gran Turismo) in a final post-process pass. This provides explicit control over the "toe" so dark areas can be aggressively crushed back to pure black while preserving bright highlights.

**Implementation (GLSL):**
```glsl
// Tune these via CVars to get the Doom 3 look
uniform float r_exposure; // e.g., 1.0
uniform float r_toe_strength; // e.g., 2.0 (higher = crushed blacks)
uniform float r_toe_length; // e.g., 0.5

vec3 UchimuraTonemap(vec3 color) {
    // Uchimura parameters
    float P = 1.0;  // max display brightness
    float a = 1.0;  // contrast
    float m = 0.22; // linear section start
    float l = 0.4;  // linear section length
    float c = r_toe_strength; // black tightness (0 to 3+) - THIS IS YOUR TOE
    float b = r_toe_length;   // pedestal

    // Apply exposure
    color *= r_exposure;

    // Evaluate curve
    float l0 = ((P - m) * l) / a;
    float L0 = m - m / a;
    float L1 = m + (1.0 - m) / a;
    float S0 = m + l0;
    float S1 = m + a * l0;
    float C2 = (a * P) / (P - S1);
    float CP = -C2 / P;

    vec3 w0 = 1.0 - smoothstep(0.0, m, color);
    vec3 w2 = step(m + l0, color);
    vec3 w1 = 1.0 - w0 - w2;

    vec3 T = m * pow(color / m, vec3(c)) + b;
    vec3 S = P - (P - S1) * exp(CP * (color - S0));
    vec3 L = m + a * (color - m);

    return T * w0 + L * w1 + S * w2;
}

void main() {
    vec3 hdrColor = texture(u_hdrBuffer, in_uv).rgb;
    
    // Tonemap to crush the toe
    vec3 mapped = UchimuraTonemap(hdrColor);
    
    // Gamma correction if output is UNORM
    // mapped = pow(mapped, vec3(1.0 / 2.2));
    
    outColor = vec4(mapped, 1.0);
}
```

## 2. Fixing the Volumetric "Blobs"

"Floating blobs" at the center of point lights are caused by the inverse-square law ($1/d^2$). When distance $d$ approaches 0 (the light's center), brightness shoots to infinity. 

### Solution: Light Clamping and Categorization

1. **Anti-Singularity Clamp:**
   Artificially widen the light source *only* for target volumetrics.
   ```glsl
   float distToLight = length(lightPos - samplePos);
   
   // DO NOT let distance go to 0. Cap it at a minimum radius.
   // Expose CVar: r_rt_volCoreRadius (e.g., 10.0 units)
   float safeDist = max(distToLight, u_volCoreRadius); 
   
   // Calculate attenuation with the safe distance
   float attenuation = 1.0 / (safeDist * safeDist);
   ```

2. **Distance-Based Volumetric Fade (Soft-start):**
   Smoothly fade out volumetric contribution near the light origin for huge ambient lights.
   ```glsl
   float coreFade = smoothstep(MIN_FADE_DIST, MAX_FADE_DIST, distToLight);
   scattering *= coreFade;
   ```

3. **Ignore "Fill" Lights:**
   Doom 3 uses point lights purely to bump ambient lighting. Use entity flags (like `noShadows`) to skip volumetric calculations for these lights entirely.

## Next Steps / Action Plan
1. Expose `r_exposure`, `r_toe_strength`, and `r_rt_volCoreRadius` as engine CVars.
2. Add the Uchimura curve to the final Tonemapping/Post composite pass shader.
3. Apply `safeDist` clamping in the Volumetrics scattering calculation.
4. Filter out non-shadowing (ambient/fill) lights from the volumetric pass.