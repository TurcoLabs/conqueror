// 326: yarım çözünürlüklü ortam örtmesinin (SSAO / ışın izlemeli AO) derinliğe duyarlı büyütmesi (Unreal'ın GTAO'su ve HDRP'nin AO'su
// gibi). Eskiden AO dokusu çift doğrusal büyütülüyordu: nesne kenarında örtü 1-2 piksel arka plana (gökyüzüne, uzaktaki zemine) taşıyor,
// AO görünümünde uzaktaki nesnelerin çevresinde gri haleler görünüyordu. AO'nun her iç dokusu tam çözünürlüklü derinliğin (2i + 1)
// dokusunda hesaplandı (SSAO.cqsh hizalaması): dört komşu doku, çift doğrusal ağırlık x bu pikselin uzaklığına yakınlıkla toplanır.
// nearFar: izdüşümün yakın / uzak düzlemi (görüş uzaklığı için)

float AOViewDepth(float d, vec2 nearFar)
{
    float z = d * 2.0 - 1.0;
    return 2.0 * nearFar.x * nearFar.y / max(nearFar.y + nearFar.x - z * (nearFar.y - nearFar.x), 1e-6);
}

// AO dokusu t'nin hesaplandığı tam çözünürlüklü derinlik dokusu
ivec2 AOSourceTexel(ivec2 t, ivec2 aoSize, ivec2 depthSize)
{
    return clamp(ivec2((vec2(t) + 0.5) * vec2(depthSize) / vec2(aoSize)), ivec2(0), depthSize - 1);
}

float SampleAOUpsampled(sampler2D ao, sampler2D depth, vec2 uv, vec2 nearFar)
{
    ivec2 fsz = textureSize(depth, 0);
    ivec2 hsz = textureSize(ao, 0);
    float dc = texelFetch(depth, clamp(ivec2(uv * vec2(fsz)), ivec2(0), fsz - 1), 0).r;
    if (dc >= 1.0)
        return 1.0;   // gökyüzü örtülmez (eskiden kenardaki nesnenin örtüsü gökyüzüne taşıyordu)
    float zc = AOViewDepth(dc, nearFar);
    vec2 p = uv * vec2(hsz) - 0.5;
    ivec2 b = ivec2(floor(p));
    vec2 f = p - vec2(b);
    float sum = 0.0, wsum = 0.0;
    float nearest = 1.0, nearestDiff = 1e30;
    for (int k = 0; k < 4; ++k)
    {
        ivec2 o = ivec2(k & 1, k >> 1);
        ivec2 t = clamp(b + o, ivec2(0), hsz - 1);
        float bw = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        float dt = texelFetch(depth, AOSourceTexel(t, hsz, fsz), 0).r;
        float diff = dt >= 1.0 ? 1e9 : abs(AOViewDepth(dt, nearFar) - zc) / (0.02 * zc + 0.02);
        float a = texelFetch(ao, t, 0).r;
        float w = (bw + 1e-3) * exp(-diff * diff);
        sum += a * w;
        wsum += w;
        if (diff < nearestDiff)
        {
            nearestDiff = diff;
            nearest = a;
        }
    }
    // Dört dokunun hiçbiri bu yüzeyde değil (bir pikselden ince nesne): uzaklığı en yakın olanın örtüsü
    return wsum > 1e-4 ? sum / wsum : nearest;
}
