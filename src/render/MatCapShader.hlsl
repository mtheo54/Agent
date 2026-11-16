// MatCapShader.hlsl — chrome satiné "Frutiger Aero" sans éclairage dynamique.
// La couleur vient d'une texture sphérique (MatCap) indexée par la normale en espace vue,
// complétée d'un liseré de Fresnel teinté selon l'état du compagnon.
// Sortie en alpha pré-multiplié (swapchain DirectComposition DXGI_ALPHA_MODE_PREMULTIPLIED).

cbuffer FrameCB : register(b0)
{
    float4x4 gViewProj;
    float4x4 gView;
    float4   gTint;    // rgb : teinte d'état (Veille, Écoute Studio, Escalade Claude, Auto-Training...)
    float4   gParams;  // x : temps (s), y : intensité de parole 0..1 (RMS), z : force du liseré, w : opacité
};

cbuffer ObjectCB : register(b1)
{
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
    float4   gPart;    // x : émissif (yeux), y : part de teinte dans le chrome
};

Texture2D    gMatCap  : register(t0);
SamplerState gSampler : register(s0);

struct VSIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;
};

struct VSOut
{
    float4 pos     : SV_Position;
    float3 viewNrm : NORMAL;
    float3 viewPos : TEXCOORD0;
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    float4 wp = mul(float4(v.pos, 1.0), gWorld);
    o.pos = mul(wp, gViewProj);
    float3 wn = normalize(mul(float4(v.nrm, 0.0), gWorldInvTranspose).xyz);
    o.viewNrm = mul(float4(wn, 0.0), gView).xyz;
    o.viewPos = mul(wp, gView).xyz;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 n = normalize(i.viewNrm);

    // Projection MatCap : (x, y) de la normale vue -> disque de la texture.
    float2 uv = float2(n.x, -n.y) * 0.495 + 0.5;
    float3 chrome = gMatCap.Sample(gSampler, uv).rgb;

    // Le chrome "prend" légèrement la couleur de l'état.
    float3 tinted = lerp(chrome, chrome * (0.55 + 0.45 * gTint.rgb) + gTint.rgb * 0.08, gPart.y);

    // Liseré de Fresnel pulsé (vue LH : la caméra regarde +Z, V pointe vers la caméra).
    float3 V = normalize(-i.viewPos);
    float fresnel = pow(1.0 - saturate(dot(n, V)), 3.0);
    float pulse = 0.85 + 0.15 * sin(gParams.x * 2.0);
    float3 color = tinted + gTint.rgb * fresnel * gParams.z * pulse;

    // Parties émissives (yeux) : s'illuminent quand il parle.
    float3 emissive = lerp(gTint.rgb, float3(1.0, 1.0, 1.0), 0.55) * (0.9 + gParams.y * 0.6);
    color = lerp(color, emissive, gPart.x);

    float a = gParams.w;
    return float4(saturate(color) * a, a);
}
