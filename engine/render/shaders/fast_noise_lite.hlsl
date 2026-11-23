// FastNoiseLite 1.1.1's HLSL (https://github.com/Auburn/FastNoiseLite, HLSL/FastNoiseLite.hlsl)
// for texture graphs' noise, with its 3D noise and warps left out and its tables in group memory
// (fnlLoadTables), or read where they are with FNL_STATIC_TABLES. The CPU twin samples the same
// version's C++ (the fastnoiselite pin in cmake/dependencies.json); keep the two at one version.
//
// MIT License
//
// Copyright(c) 2023 Jordan Peck (jordan.me2@gmail.com)
// Copyright(c) 2023 Contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// Switch between using floats or doubles for input position
typedef float FNLfloat;
//typedef double FNLfloat;

// Noise Type
#define FNL_NOISE_OPENSIMPLEX2 0
#define FNL_NOISE_OPENSIMPLEX2S 1
#define FNL_NOISE_CELLULAR 2
#define FNL_NOISE_PERLIN 3
#define FNL_NOISE_VALUE_CUBIC 4
#define FNL_NOISE_VALUE 5
typedef int fnl_noise_type;

// Rotation types
#define FNL_ROTATION_NONE 0
#define FNL_ROTATION_IMPROVE_XY_PLANES 1
#define FNL_ROTATION_IMPROVE_XZ_PLANES 2
typedef int fnl_rotation_type_3d;

// Fractal types
#define FNL_FRACTAL_NONE 0
#define FNL_FRACTAL_FBM 1
#define FNL_FRACTAL_RIDGED 2
#define FNL_FRACTAL_PINGPONG 3
#define FNL_FRACTAL_DOMAIN_WARP_PROGRESSIVE 4
#define FNL_FRACTAL_DOMAIN_WARP_INDEPENDENT 5
typedef int fnl_fractal_type;

#define FNL_CELLULAR_DISTANCE_EUCLIDEAN 0
#define FNL_CELLULAR_DISTANCE_EUCLIDEANSQ 1
#define FNL_CELLULAR_DISTANCE_MANHATTAN 2
#define FNL_CELLULAR_DISTANCE_HYBRID 3
typedef int fnl_cellular_distance_func;

#define FNL_CELLULAR_RETURN_TYPE_CELLVALUE 0
#define FNL_CELLULAR_RETURN_TYPE_DISTANCE 1
#define FNL_CELLULAR_RETURN_TYPE_DISTANCE2 2
#define FNL_CELLULAR_RETURN_TYPE_DISTANCE2ADD 3
#define FNL_CELLULAR_RETURN_TYPE_DISTANCE2SUB 4
#define FNL_CELLULAR_RETURN_TYPE_DISTANCE2MUL 5
#define FNL_CELLULAR_RETURN_TYPE_DISTANCE2DIV 6
typedef int fnl_cellular_return_type;

#define FNL_DOMAIN_WARP_OPENSIMPLEX2 0
#define FNL_DOMAIN_WARP_OPENSIMPLEX2_REDUCED 1
#define FNL_DOMAIN_WARP_BASICGRID 2
typedef int fnl_domain_warp_type;

// Removes [0x80004005 - unknown error] 'internal error: no storage type for block output' 
#if UNITY_VERSION
    #define FNL_FLATTEN [flatten]
    #define FNL_UNROLL [unroll(1)]
#else
    #define FNL_FLATTEN
    #define FNL_UNROLL
#endif

// The kinds of noise, fractal and domain warp compiled, a bit for each FNL_NOISE_*,
// FNL_FRACTAL_* and FNL_DOMAIN_WARP_* value: a live texture graph's shader defines them to the
// kinds its graph uses, so drivers compile only those; the bake compiles every kind.
#ifndef FNL_USED_NOISE
#define FNL_USED_NOISE 63
#endif
#ifndef FNL_USED_FRACTAL
#define FNL_USED_FRACTAL 63
#endif
#ifndef FNL_USED_WARP
#define FNL_USED_WARP 7
#endif
// Windows' HLSL compiler takes no shifts in #if, so a kind's bit is worked out by comparisons
// and tested by division.
#define FNL_BIT(kind)                                                                              \
    (((kind) == 0) * 1 + ((kind) == 1) * 2 + ((kind) == 2) * 4 + ((kind) == 3) * 8 +               \
     ((kind) == 4) * 16 + ((kind) == 5) * 32)
#define FNL_USES(used, kind) ((used) / FNL_BIT(kind) - (used) / (FNL_BIT(kind) * 2) * 2 != 0)

/**
 * Structure containing entire noise system state.
 * @note Must only be created using fnlCreateState(optional: seed). To ensure defaults are set.
 */
struct fnl_state
{
    /**
     * Seed used for all noise types.
     * @remark Default: 1337
     */
    int seed;

    /**
     * The frequency for all noise types.
     * @remark Default: 0.01
     */
    float frequency;

    /**
     * The noise algorithm to be used by GetNoise(...).
     * @remark Default: FNL_NOISE_OPENSIMPLEX2
     */
    fnl_noise_type noise_type;

    /**
     * Sets noise rotation type for 3D.
     * @remark Default: FNL_ROTATION_NONE
     */
    fnl_rotation_type_3d rotation_type_3d;

    /**
     * The method used for combining octaves for all fractal noise types.
     * @remark Default: None
     * @remark FNL_FRACTAL_DOMAIN_WARP_... only effects fnlDomainWarp...
     */
    fnl_fractal_type fractal_type;

    /**
     * The octave count for all fractal noise types.
     * @remark Default: 3
     */
    int octaves;

    /**
     * The octave lacunarity for all fractal noise types.
     * @remark Default: 2.0
     */
    float lacunarity;

    /**
     * The octave gain for all fractal noise types.
     * @remark Default: 0.5
     */
    float gain;

    /**
     * The octave weighting for all none Domaain Warp fractal types.
     * @remark Default: 0.0
     * @remark 
     */
    float weighted_strength;

    /**
     * The strength of the fractal ping pong effect.
     * @remark Default: 2.0
     */
    float ping_pong_strength;

    /**
     * The distance function used in cellular noise calculations.
     * @remark Default: FNL_CELLULAR_DISTANCE_EUCLIDEANSQ
     */
    fnl_cellular_distance_func cellular_distance_func;

    /**
     * The cellular return type from cellular noise calculations.
     * @remark Default: FNL_CELLULAR_RETURN_TYPE_DISTANCE
     */
    fnl_cellular_return_type cellular_return_type;

    /**
     * The maximum distance a cellular point can move from it's grid position.
     * @remark Default: 1.0
     * @note Setting this higher than 1 will cause artifacts.
     */
    float cellular_jitter_mod;

    /**
     * The warp algorithm when using fnlDomainWarp...
     * @remark Default: OpenSimplex2
     */
    fnl_domain_warp_type domain_warp_type;

    /**
     * The maximum warp distance from original position when using fnlDomainWarp...
     * @remark Default: 1.0
     */
    float domain_warp_amp;
};

/**
 * Creates a noise state with default values.
 * @param seed Optionally set the state seed.
 */
fnl_state fnlCreateState(int seed);

/**
 * 2D noise at given position using the state settings
 * @returns Noise output bounded between -1 and 1.
 */
float fnlGetNoise2D(fnl_state state, FNLfloat x, FNLfloat y);

/**
 * 2D warps the input position using current domain warp settings.
 * 
 * Example usage with fnlGetNoise2D:
 * ```
 * fnlDomainWarp2D(state, x, y);
 * noise = fnlGetNoise2D(state, x, y);
 * ```
 */
void fnlDomainWarp2D(fnl_state state, inout FNLfloat x, inout FNLfloat y);

// From here on, this is private implementation

// Constants

static const float FNL_GRADIENTS_2D_TABLE[256] = 
{
    0.130526192220052f, 0.99144486137381f, 0.38268343236509f, 0.923879532511287f, 0.608761429008721f, 0.793353340291235f, 0.793353340291235f, 0.608761429008721f,
    0.923879532511287f, 0.38268343236509f, 0.99144486137381f, 0.130526192220051f, 0.99144486137381f, -0.130526192220051f, 0.923879532511287f, -0.38268343236509f,
    0.793353340291235f, -0.60876142900872f, 0.608761429008721f, -0.793353340291235f, 0.38268343236509f, -0.923879532511287f, 0.130526192220052f, -0.99144486137381f,
    -0.130526192220052f, -0.99144486137381f, -0.38268343236509f, -0.923879532511287f, -0.608761429008721f, -0.793353340291235f, -0.793353340291235f, -0.608761429008721f,
    -0.923879532511287f, -0.38268343236509f, -0.99144486137381f, -0.130526192220052f, -0.99144486137381f, 0.130526192220051f, -0.923879532511287f, 0.38268343236509f,
    -0.793353340291235f, 0.608761429008721f, -0.608761429008721f, 0.793353340291235f, -0.38268343236509f, 0.923879532511287f, -0.130526192220052f, 0.99144486137381f,
    0.130526192220052f, 0.99144486137381f, 0.38268343236509f, 0.923879532511287f, 0.608761429008721f, 0.793353340291235f, 0.793353340291235f, 0.608761429008721f,
    0.923879532511287f, 0.38268343236509f, 0.99144486137381f, 0.130526192220051f, 0.99144486137381f, -0.130526192220051f, 0.923879532511287f, -0.38268343236509f,
    0.793353340291235f, -0.60876142900872f, 0.608761429008721f, -0.793353340291235f, 0.38268343236509f, -0.923879532511287f, 0.130526192220052f, -0.99144486137381f,
    -0.130526192220052f, -0.99144486137381f, -0.38268343236509f, -0.923879532511287f, -0.608761429008721f, -0.793353340291235f, -0.793353340291235f, -0.608761429008721f,
    -0.923879532511287f, -0.38268343236509f, -0.99144486137381f, -0.130526192220052f, -0.99144486137381f, 0.130526192220051f, -0.923879532511287f, 0.38268343236509f,
    -0.793353340291235f, 0.608761429008721f, -0.608761429008721f, 0.793353340291235f, -0.38268343236509f, 0.923879532511287f, -0.130526192220052f, 0.99144486137381f,
    0.130526192220052f, 0.99144486137381f, 0.38268343236509f, 0.923879532511287f, 0.608761429008721f, 0.793353340291235f, 0.793353340291235f, 0.608761429008721f,
    0.923879532511287f, 0.38268343236509f, 0.99144486137381f, 0.130526192220051f, 0.99144486137381f, -0.130526192220051f, 0.923879532511287f, -0.38268343236509f,
    0.793353340291235f, -0.60876142900872f, 0.608761429008721f, -0.793353340291235f, 0.38268343236509f, -0.923879532511287f, 0.130526192220052f, -0.99144486137381f,
    -0.130526192220052f, -0.99144486137381f, -0.38268343236509f, -0.923879532511287f, -0.608761429008721f, -0.793353340291235f, -0.793353340291235f, -0.608761429008721f,
    -0.923879532511287f, -0.38268343236509f, -0.99144486137381f, -0.130526192220052f, -0.99144486137381f, 0.130526192220051f, -0.923879532511287f, 0.38268343236509f,
    -0.793353340291235f, 0.608761429008721f, -0.608761429008721f, 0.793353340291235f, -0.38268343236509f, 0.923879532511287f, -0.130526192220052f, 0.99144486137381f,
    0.130526192220052f, 0.99144486137381f, 0.38268343236509f, 0.923879532511287f, 0.608761429008721f, 0.793353340291235f, 0.793353340291235f, 0.608761429008721f,
    0.923879532511287f, 0.38268343236509f, 0.99144486137381f, 0.130526192220051f, 0.99144486137381f, -0.130526192220051f, 0.923879532511287f, -0.38268343236509f,
    0.793353340291235f, -0.60876142900872f, 0.608761429008721f, -0.793353340291235f, 0.38268343236509f, -0.923879532511287f, 0.130526192220052f, -0.99144486137381f,
    -0.130526192220052f, -0.99144486137381f, -0.38268343236509f, -0.923879532511287f, -0.608761429008721f, -0.793353340291235f, -0.793353340291235f, -0.608761429008721f,
    -0.923879532511287f, -0.38268343236509f, -0.99144486137381f, -0.130526192220052f, -0.99144486137381f, 0.130526192220051f, -0.923879532511287f, 0.38268343236509f,
    -0.793353340291235f, 0.608761429008721f, -0.608761429008721f, 0.793353340291235f, -0.38268343236509f, 0.923879532511287f, -0.130526192220052f, 0.99144486137381f,
    0.130526192220052f, 0.99144486137381f, 0.38268343236509f, 0.923879532511287f, 0.608761429008721f, 0.793353340291235f, 0.793353340291235f, 0.608761429008721f,
    0.923879532511287f, 0.38268343236509f, 0.99144486137381f, 0.130526192220051f, 0.99144486137381f, -0.130526192220051f, 0.923879532511287f, -0.38268343236509f,
    0.793353340291235f, -0.60876142900872f, 0.608761429008721f, -0.793353340291235f, 0.38268343236509f, -0.923879532511287f, 0.130526192220052f, -0.99144486137381f,
    -0.130526192220052f, -0.99144486137381f, -0.38268343236509f, -0.923879532511287f, -0.608761429008721f, -0.793353340291235f, -0.793353340291235f, -0.608761429008721f,
    -0.923879532511287f, -0.38268343236509f, -0.99144486137381f, -0.130526192220052f, -0.99144486137381f, 0.130526192220051f, -0.923879532511287f, 0.38268343236509f,
    -0.793353340291235f, 0.608761429008721f, -0.608761429008721f, 0.793353340291235f, -0.38268343236509f, 0.923879532511287f, -0.130526192220052f, 0.99144486137381f,
    0.38268343236509f, 0.923879532511287f, 0.923879532511287f, 0.38268343236509f, 0.923879532511287f, -0.38268343236509f, 0.38268343236509f, -0.923879532511287f,
    -0.38268343236509f, -0.923879532511287f, -0.923879532511287f, -0.38268343236509f, -0.923879532511287f, 0.38268343236509f, -0.38268343236509f, 0.923879532511287f,
};

static const float FNL_RAND_VECS_2D_TABLE[512] = 
{
    -0.2700222198f, -0.9628540911f, 0.3863092627f, -0.9223693152f, 0.04444859006f, -0.999011673f, -0.5992523158f, -0.8005602176f, -0.7819280288f, 0.6233687174f, 0.9464672271f, 0.3227999196f, -0.6514146797f, -0.7587218957f, 0.9378472289f, 0.347048376f,
    -0.8497875957f, -0.5271252623f, -0.879042592f, 0.4767432447f, -0.892300288f, -0.4514423508f, -0.379844434f, -0.9250503802f, -0.9951650832f, 0.0982163789f, 0.7724397808f, -0.6350880136f, 0.7573283322f, -0.6530343002f, -0.9928004525f, -0.119780055f,
    -0.0532665713f, 0.9985803285f, 0.9754253726f, -0.2203300762f, -0.7665018163f, 0.6422421394f, 0.991636706f, 0.1290606184f, -0.994696838f, 0.1028503788f, -0.5379205513f, -0.84299554f, 0.5022815471f, -0.8647041387f, 0.4559821461f, -0.8899889226f,
    -0.8659131224f, -0.5001944266f, 0.0879458407f, -0.9961252577f, -0.5051684983f, 0.8630207346f, 0.7753185226f, -0.6315704146f, -0.6921944612f, 0.7217110418f, -0.5191659449f, -0.8546734591f, 0.8978622882f, -0.4402764035f, -0.1706774107f, 0.9853269617f,
    -0.9353430106f, -0.3537420705f, -0.9992404798f, 0.03896746794f, -0.2882064021f, -0.9575683108f, -0.9663811329f, 0.2571137995f, -0.8759714238f, -0.4823630009f, -0.8303123018f, -0.5572983775f, 0.05110133755f, -0.9986934731f, -0.8558373281f, -0.5172450752f,
    0.09887025282f, 0.9951003332f, 0.9189016087f, 0.3944867976f, -0.2439375892f, -0.9697909324f, -0.8121409387f, -0.5834613061f, -0.9910431363f, 0.1335421355f, 0.8492423985f, -0.5280031709f, -0.9717838994f, -0.2358729591f, 0.9949457207f, 0.1004142068f,
    0.6241065508f, -0.7813392434f, 0.662910307f, 0.7486988212f, -0.7197418176f, 0.6942418282f, -0.8143370775f, -0.5803922158f, 0.104521054f, -0.9945226741f, -0.1065926113f, -0.9943027784f, 0.445799684f, -0.8951327509f, 0.105547406f, 0.9944142724f,
    -0.992790267f, 0.1198644477f, -0.8334366408f, 0.552615025f, 0.9115561563f, -0.4111755999f, 0.8285544909f, -0.5599084351f, 0.7217097654f, -0.6921957921f, 0.4940492677f, -0.8694339084f, -0.3652321272f, -0.9309164803f, -0.9696606758f, 0.2444548501f,
    0.08925509731f, -0.996008799f, 0.5354071276f, -0.8445941083f, -0.1053576186f, 0.9944343981f, -0.9890284586f, 0.1477251101f, 0.004856104961f, 0.9999882091f, 0.9885598478f, 0.1508291331f, 0.9286129562f, -0.3710498316f, -0.5832393863f, -0.8123003252f,
    0.3015207509f, 0.9534596146f, -0.9575110528f, 0.2883965738f, 0.9715802154f, -0.2367105511f, 0.229981792f, 0.9731949318f, 0.955763816f, -0.2941352207f, 0.740956116f, 0.6715534485f, -0.9971513787f, -0.07542630764f, 0.6905710663f, -0.7232645452f,
    -0.290713703f, -0.9568100872f, 0.5912777791f, -0.8064679708f, -0.9454592212f, -0.325740481f, 0.6664455681f, 0.74555369f, 0.6236134912f, 0.7817328275f, 0.9126993851f, -0.4086316587f, -0.8191762011f, 0.5735419353f, -0.8812745759f, -0.4726046147f,
    0.9953313627f, 0.09651672651f, 0.9855650846f, -0.1692969699f, -0.8495980887f, 0.5274306472f, 0.6174853946f, -0.7865823463f, 0.8508156371f, 0.52546432f, 0.9985032451f, -0.05469249926f, 0.1971371563f, -0.9803759185f, 0.6607855748f, -0.7505747292f,
    -0.03097494063f, 0.9995201614f, -0.6731660801f, 0.739491331f, -0.7195018362f, -0.6944905383f, 0.9727511689f, 0.2318515979f, 0.9997059088f, -0.0242506907f, 0.4421787429f, -0.8969269532f, 0.9981350961f, -0.061043673f, -0.9173660799f, -0.3980445648f,
    -0.8150056635f, -0.5794529907f, -0.8789331304f, 0.4769450202f, 0.0158605829f, 0.999874213f, -0.8095464474f, 0.5870558317f, -0.9165898907f, -0.3998286786f, -0.8023542565f, 0.5968480938f, -0.5176737917f, 0.8555780767f, -0.8154407307f, -0.5788405779f,
    0.4022010347f, -0.9155513791f, -0.9052556868f, -0.4248672045f, 0.7317445619f, 0.6815789728f, -0.5647632201f, -0.8252529947f, -0.8403276335f, -0.5420788397f, -0.9314281527f, 0.363925262f, 0.5238198472f, 0.8518290719f, 0.7432803869f, -0.6689800195f,
    -0.985371561f, -0.1704197369f, 0.4601468731f, 0.88784281f, 0.825855404f, 0.5638819483f, 0.6182366099f, 0.7859920446f, 0.8331502863f, -0.553046653f, 0.1500307506f, 0.9886813308f, -0.662330369f, -0.7492119075f, -0.668598664f, 0.743623444f,
    0.7025606278f, 0.7116238924f, -0.5419389763f, -0.8404178401f, -0.3388616456f, 0.9408362159f, 0.8331530315f, 0.5530425174f, -0.2989720662f, -0.9542618632f, 0.2638522993f, 0.9645630949f, 0.124108739f, -0.9922686234f, -0.7282649308f, -0.6852956957f,
    0.6962500149f, 0.7177993569f, -0.9183535368f, 0.3957610156f, -0.6326102274f, -0.7744703352f, -0.9331891859f, -0.359385508f, -0.1153779357f, -0.9933216659f, 0.9514974788f, -0.3076565421f, -0.08987977445f, -0.9959526224f, 0.6678496916f, 0.7442961705f,
    0.7952400393f, -0.6062947138f, -0.6462007402f, -0.7631674805f, -0.2733598753f, 0.9619118351f, 0.9669590226f, -0.254931851f, -0.9792894595f, 0.2024651934f, -0.5369502995f, -0.8436138784f, -0.270036471f, -0.9628500944f, -0.6400277131f, 0.7683518247f,
    -0.7854537493f, -0.6189203566f, 0.06005905383f, -0.9981948257f, -0.02455770378f, 0.9996984141f, -0.65983623f, 0.751409442f, -0.6253894466f, -0.7803127835f, -0.6210408851f, -0.7837781695f, 0.8348888491f, 0.5504185768f, -0.1592275245f, 0.9872419133f,
    0.8367622488f, 0.5475663786f, -0.8675753916f, -0.4973056806f, -0.2022662628f, -0.9793305667f, 0.9399189937f, 0.3413975472f, 0.9877404807f, -0.1561049093f, -0.9034455656f, 0.4287028224f, 0.1269804218f, -0.9919052235f, -0.3819600854f, 0.924178821f,
    0.9754625894f, 0.2201652486f, -0.3204015856f, -0.9472818081f, -0.9874760884f, 0.1577687387f, 0.02535348474f, -0.9996785487f, 0.4835130794f, -0.8753371362f, -0.2850799925f, -0.9585037287f, -0.06805516006f, -0.99768156f, -0.7885244045f, -0.6150034663f,
    0.3185392127f, -0.9479096845f, 0.8880043089f, 0.4598351306f, 0.6476921488f, -0.7619021462f, 0.9820241299f, 0.1887554194f, 0.9357275128f, -0.3527237187f, -0.8894895414f, 0.4569555293f, 0.7922791302f, 0.6101588153f, 0.7483818261f, 0.6632681526f,
    -0.7288929755f, -0.6846276581f, 0.8729032783f, -0.4878932944f, 0.8288345784f, 0.5594937369f, 0.08074567077f, 0.9967347374f, 0.9799148216f, -0.1994165048f, -0.580730673f, -0.8140957471f, -0.4700049791f, -0.8826637636f, 0.2409492979f, 0.9705377045f,
    0.9437816757f, -0.3305694308f, -0.8927998638f, -0.4504535528f, -0.8069622304f, 0.5906030467f, 0.06258973166f, 0.9980393407f, -0.9312597469f, 0.3643559849f, 0.5777449785f, 0.8162173362f, -0.3360095855f, -0.941858566f, 0.697932075f, -0.7161639607f,
    -0.002008157227f, -0.9999979837f, -0.1827294312f, -0.9831632392f, -0.6523911722f, 0.7578824173f, -0.4302626911f, -0.9027037258f, -0.9985126289f, -0.05452091251f, -0.01028102172f, -0.9999471489f, -0.4946071129f, 0.8691166802f, -0.2999350194f, 0.9539596344f,
    0.8165471961f, 0.5772786819f, 0.2697460475f, 0.962931498f, -0.7306287391f, -0.6827749597f, -0.7590952064f, -0.6509796216f, -0.907053853f, 0.4210146171f, -0.5104861064f, -0.8598860013f, 0.8613350597f, 0.5080373165f, 0.5007881595f, -0.8655698812f,
    -0.654158152f, 0.7563577938f, -0.8382755311f, -0.545246856f, 0.6940070834f, 0.7199681717f, 0.06950936031f, 0.9975812994f, 0.1702942185f, -0.9853932612f, 0.2695973274f, 0.9629731466f, 0.5519612192f, -0.8338697815f, 0.225657487f, -0.9742067022f,
    0.4215262855f, -0.9068161835f, 0.4881873305f, -0.8727388672f, -0.3683854996f, -0.9296731273f, -0.9825390578f, 0.1860564427f, 0.81256471f, 0.5828709909f, 0.3196460933f, -0.9475370046f, 0.9570913859f, 0.2897862643f, -0.6876655497f, -0.7260276109f,
    -0.9988770922f, -0.047376731f, -0.1250179027f, 0.992154486f, -0.8280133617f, 0.560708367f, 0.9324863769f, -0.3612051451f, 0.6394653183f, 0.7688199442f, -0.01623847064f, -0.9998681473f, -0.9955014666f, -0.09474613458f, -0.81453315f, 0.580117012f,
    0.4037327978f, -0.9148769469f, 0.9944263371f, 0.1054336766f, -0.1624711654f, 0.9867132919f, -0.9949487814f, -0.100383875f, -0.6995302564f, 0.7146029809f, 0.5263414922f, -0.85027327f, -0.5395221479f, 0.841971408f, 0.6579370318f, 0.7530729462f,
    0.01426758847f, -0.9998982128f, -0.6734383991f, 0.7392433447f, 0.639412098f, -0.7688642071f, 0.9211571421f, 0.3891908523f, -0.146637214f, -0.9891903394f, -0.782318098f, 0.6228791163f, -0.5039610839f, -0.8637263605f, -0.7743120191f, -0.6328039957f,
};

#ifdef FNL_STATIC_TABLES
// A fragment shader has no group memory, so live texture graphs (FNL_STATIC_TABLES) read the
// constant tables themselves.
#define GRADIENTS_2D FNL_GRADIENTS_2D_TABLE
#define RAND_VECS_2D FNL_RAND_VECS_2D_TABLE
#else
// The tables, copied into group memory by fnlLoadTables before any noise is sampled: indexing a
// constant array at run time makes some compilers (Tint) copy the whole array at every lookup.
groupshared float GRADIENTS_2D[256];
groupshared float RAND_VECS_2D[512];

// Each of a group's threads copies its share of the tables; a group memory barrier must follow.
void fnlLoadTables(uint thread, uint threads)
{
    for (uint i = thread; i < 256u; i += threads)
    {
        GRADIENTS_2D[i] = FNL_GRADIENTS_2D_TABLE[i];
    }
    for (uint j = thread; j < 512u; j += threads)
    {
        RAND_VECS_2D[j] = FNL_RAND_VECS_2D_TABLE[j];
    }
}
#endif

// Utilities

static inline float _fnlFastMin(float x, float y) { return x < y ? x : y; }

static inline float _fnlFastMax(float x, float y) { return x > y ? x : y; }

static inline float _fnlFastAbs(float f) { return f < 0 ? -f : f; }

static inline float _fnlFastSqrt(float a) { return sqrt(a); }

static inline int _fnlFastFloor(FNLfloat f) { return (f >= 0 ? (int)f : (int)f - 1); }

static inline int _fnlFastRound(FNLfloat f) { return (f >= 0) ? (int)(f + 0.5f) : (int)(f - 0.5f); }

static inline float _fnlLerp(float a, float b, float t) { return a + t * (b - a); }

static inline float _fnlInterpHermite(float t) { return t * t * (3 - 2 * t); }

static inline float _fnlInterpQuintic(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }

static inline float _fnlCubicLerp(float a, float b, float c, float d, float t)
{
    float p = (d - c) - (a - b);
    return t * t * t * p + t * t * ((a - b) - p) + t * (c - a) + b;
}

static inline float _fnlPingPong(float t)
{
    t -= (int)(t * 0.5f) * 2;
    return t < 1 ? t : 2 - t;
}

static float _fnlCalculateFractalBounding(fnl_state state)
{
    float gain = _fnlFastAbs(state.gain);
    float amp = gain;
    float ampFractal = 1.0f;
    for (int i = 1; i < state.octaves; i++)
    {
        ampFractal += amp;
        amp *= gain;
    }
    return 1.0f / ampFractal;
}

// Hashing

static const int PRIME_X = 501125321;
static const int PRIME_Y = 1136930381;
static const int PRIME_Z = 1720413743;

static inline int _fnlHash2D(int seed, int xPrimed, int yPrimed)
{
    int hash = seed ^ xPrimed ^ yPrimed;

    hash *= 0x27d4eb2d;
    return hash;
}

static inline float _fnlValCoord2D(int seed, int xPrimed, int yPrimed)
{
    int hash = _fnlHash2D(seed, xPrimed, yPrimed);
    hash *= hash;
    hash ^= hash << 19;
    return hash * (1 / 2147483648.0f);
}

static inline float _fnlGradCoord2D(int seed, int xPrimed, int yPrimed, float xd, float yd)
{
    int hash = _fnlHash2D(seed, xPrimed, yPrimed);
    hash ^= hash >> 15;
    hash &= 127 << 1;
    return xd * GRADIENTS_2D[hash] + yd * GRADIENTS_2D[hash | 1];
}

static inline void _fnlGradCoordOut2D(int seed, int xPrimed, int yPrimed, out float xo, out float yo)
{
    int hash = _fnlHash2D(seed, xPrimed, yPrimed) & (255 << 1);

    xo = RAND_VECS_2D[hash];
    yo = RAND_VECS_2D[hash | 1];
}

static inline void _fnlGradCoordDual2D(int seed, int xPrimed, int yPrimed, float xd, float yd, out float xo, out float yo)
{
    int hash = _fnlHash2D(seed, xPrimed, yPrimed);
    int index1 = hash & (127 << 1);
    int index2 = (hash >> 7) & (255 << 1);

    float xg = GRADIENTS_2D[index1];
    float yg = GRADIENTS_2D[index1 | 1];
    float value = xd * xg + yd * yg;

    float xgo = RAND_VECS_2D[index2];
    float ygo = RAND_VECS_2D[index2 | 1];

    xo = value * xgo;
    yo = value * ygo;
}

// Generic Noise Gen

static float _fnlSingleSimplex2D(int seed, FNLfloat x, FNLfloat y);
static float _fnlSingleOpenSimplex2S2D(int seed, FNLfloat x, FNLfloat y);
static float _fnlSingleCellular2D(fnl_state state, int seed, FNLfloat x, FNLfloat y);
static float _fnlSinglePerlin2D(int seed, FNLfloat x, FNLfloat y);
static float _fnlSingleValueCubic2D(int seed, FNLfloat x, FNLfloat y);
static float _fnlSingleValue2D(int seed, FNLfloat x, FNLfloat y);

static float _fnlGenNoiseSingle2D(fnl_state state, int seed, FNLfloat x, FNLfloat y)
{
    FNL_FLATTEN switch (state.noise_type)
    {
#if FNL_USES(FNL_USED_NOISE, FNL_NOISE_OPENSIMPLEX2)
    case FNL_NOISE_OPENSIMPLEX2:
        return _fnlSingleSimplex2D(seed, x, y);
#endif
#if FNL_USES(FNL_USED_NOISE, FNL_NOISE_OPENSIMPLEX2S)
    case FNL_NOISE_OPENSIMPLEX2S:
        return _fnlSingleOpenSimplex2S2D(seed, x, y);
#endif
#if FNL_USES(FNL_USED_NOISE, FNL_NOISE_CELLULAR)
    case FNL_NOISE_CELLULAR:
        return _fnlSingleCellular2D(state, seed, x, y);
#endif
#if FNL_USES(FNL_USED_NOISE, FNL_NOISE_PERLIN)
    case FNL_NOISE_PERLIN:
        return _fnlSinglePerlin2D(seed, x, y);
#endif
#if FNL_USES(FNL_USED_NOISE, FNL_NOISE_VALUE_CUBIC)
    case FNL_NOISE_VALUE_CUBIC:
        return _fnlSingleValueCubic2D(seed, x, y);
#endif
#if FNL_USES(FNL_USED_NOISE, FNL_NOISE_VALUE)
    case FNL_NOISE_VALUE:
        return _fnlSingleValue2D(seed, x, y);
#endif
    default:
        return 0;
    }
}

// Noise Coordinate Transforms (frequency, and possible skew or rotation)

static void _fnlTransformNoiseCoordinate2D(fnl_state state, inout FNLfloat x, inout FNLfloat y)
{
    x *= state.frequency;
    y *= state.frequency;

    switch (state.noise_type)
    {
    case FNL_NOISE_OPENSIMPLEX2:
    case FNL_NOISE_OPENSIMPLEX2S:
    {
        const FNLfloat SQRT3 = (FNLfloat)1.7320508075688772935274463415059;
        const FNLfloat F2 = 0.5f * (SQRT3 - 1);
        FNLfloat t = (x + y) * F2;
        x += t;
        y += t;
    }
    break;
    default:
        break;
    }
}

// Domain Warp Coordinate Transforms

static void _fnlTransformDomainWarpCoordinate2D(fnl_state state, inout FNLfloat x, inout FNLfloat y)
{
    switch (state.domain_warp_type)
    {
    case FNL_DOMAIN_WARP_OPENSIMPLEX2:
    case FNL_DOMAIN_WARP_OPENSIMPLEX2_REDUCED:
    {
        const FNLfloat SQRT3 = (FNLfloat)1.7320508075688772935274463415059;
        const FNLfloat F2 = 0.5f * (SQRT3 - 1);
        FNLfloat t = (x + y) * F2;
        x += t;
        y += t;
    }
    break;
    default:
        break;
    }
}

// Fractal FBm

static float _fnlGenFractalFBM2D(fnl_state state, FNLfloat x, FNLfloat y)
{
    int seed = state.seed;
    float sum = 0;
    float amp = _fnlCalculateFractalBounding(state);

    for (int i = 0; i < state.octaves; i++)
    {
        float noise = _fnlGenNoiseSingle2D(state, seed++, x, y);
        sum += noise * amp;
        amp *= _fnlLerp(1.0f, _fnlFastMin(noise + 1, 2) * 0.5f, state.weighted_strength);

        x *= state.lacunarity;
        y *= state.lacunarity;
        amp *= state.gain;
    }

    return sum;
}

// Fractal Ridged

static float _fnlGenFractalRidged2D(fnl_state state, FNLfloat x, FNLfloat y)
{
    int seed = state.seed;
    float sum = 0;
    float amp = _fnlCalculateFractalBounding(state);

    for (int i = 0; i < state.octaves; i++)
    {
        float noise = _fnlFastAbs(_fnlGenNoiseSingle2D(state, seed++, x, y));
        sum += (noise * -2 + 1) * amp;
        amp *= _fnlLerp(1.0f, 1 - noise, state.weighted_strength);

        x *= state.lacunarity;
        y *= state.lacunarity;
        amp *= state.gain;
    }

    return sum;
}

// Fractal PingPong

static float _fnlGenFractalPingPong2D(fnl_state state, FNLfloat x, FNLfloat y)
{
    int seed = state.seed;
    float sum = 0;
    float amp = _fnlCalculateFractalBounding(state);

    for (int i = 0; i < state.octaves; i++)
    {
        float noise = _fnlPingPong((_fnlGenNoiseSingle2D(state, seed++, x, y) + 1) * state.ping_pong_strength);
        sum += (noise - 0.5f) * 2 * amp;
        amp *= _fnlLerp(1.0f, noise, state.weighted_strength);

        x *= state.lacunarity;
        y *= state.lacunarity;
        amp *= state.gain;
    }

    return sum;
}

// Simplex/OpenSimplex2 Noise

static float _fnlSingleSimplex2D(int seed, FNLfloat x, FNLfloat y)
{
    // 2D OpenSimplex2 case uses the same algorithm as ordinary Simplex.

    const float SQRT3 = 1.7320508075688772935274463415059f;
    const float G2 = (3 - SQRT3) / 6;

    /*
     * --- Skew moved to TransformNoiseCoordinate method ---
     * const FNLfloat F2 = 0.5f * (SQRT3 - 1);
     * FNLfloat s = (x + y) * F2;
     * x += s; y += s;
     */

    int i = _fnlFastFloor(x);
    int j = _fnlFastFloor(y);
    float xi = (float)(x - i);
    float yi = (float)(y - j);

    float t = (xi + yi) * G2;
    float x0 = (float)(xi - t);
    float y0 = (float)(yi - t);

    i *= PRIME_X;
    j *= PRIME_Y;

    float n0, n1, n2;

    float a = 0.5f - x0 * x0 - y0 * y0;
    if (a <= 0)
        n0 = 0;
    else
    {
        n0 = (a * a) * (a * a) * _fnlGradCoord2D(seed, i, j, x0, y0);
    }

    float c = (float)(2 * (1 - 2 * G2) * (1 / G2 - 2)) * t + ((float)(-2 * (1 - 2 * G2) * (1 - 2 * G2)) + a);
    if (c <= 0)
        n2 = 0;
    else
    {
        float x2 = x0 + (2 * (float)G2 - 1);
        float y2 = y0 + (2 * (float)G2 - 1);
        n2 = (c * c) * (c * c) * _fnlGradCoord2D(seed, i + PRIME_X, j + PRIME_Y, x2, y2);
    }

    if (y0 > x0)
    {
        float x1 = x0 + (float)G2;
        float y1 = y0 + ((float)G2 - 1);
        float b = 0.5f - x1 * x1 - y1 * y1;
        if (b <= 0)
            n1 = 0;
        else
        {
            n1 = (b * b) * (b * b) * _fnlGradCoord2D(seed, i, j + PRIME_Y, x1, y1);
        }
    }
    else
    {
        float x1 = x0 + ((float)G2 - 1);
        float y1 = y0 + (float)G2;
        float b = 0.5f - x1 * x1 - y1 * y1;
        if (b <= 0)
            n1 = 0;
        else
        {
            n1 = (b * b) * (b * b) * _fnlGradCoord2D(seed, i + PRIME_X, j, x1, y1);
        }
    }

    return (n0 + n1 + n2) * 99.83685446303647f;
}

// OpenSimplex2S Noise

static float _fnlSingleOpenSimplex2S2D(int seed, FNLfloat x, FNLfloat y)
{
    // 2D OpenSimplex2S case is a modified 2D simplex noise.

    const FNLfloat SQRT3 = (FNLfloat)1.7320508075688772935274463415059;
    const FNLfloat G2 = (3 - SQRT3) / 6;

    /*
     * --- Skew moved to TransformNoiseCoordinate method ---
     * const FNLfloat F2 = 0.5f * (SQRT3 - 1);
     * FNLfloat s = (x + y) * F2;
     * x += s; y += s;
    */

    int i = _fnlFastFloor(x);
    int j = _fnlFastFloor(y);
    float xi = (float)(x - i);
    float yi = (float)(y - j);

    i *= PRIME_X;
    j *= PRIME_Y;
    int i1 = i + PRIME_X;
    int j1 = j + PRIME_Y;

    float t = (xi + yi) * (float)G2;
    float x0 = xi - t;
    float y0 = yi - t;

    int aMask = (int)((xi + yi + 1) * -0.5f);
    int bMask = (int)((xi - (aMask + 2)) * 0.5f - yi);
    int cMask = (int)((yi - (aMask + 2)) * 0.5f - xi);

    float a0 = (2.0f / 3.0f) - x0 * x0 - y0 * y0;
    float value = (a0 * a0) * (a0 * a0) * _fnlGradCoord2D(seed, i, j, x0, y0);

    float a1 = (float)(2 * (1 - 2 * G2) * (1 / G2 - 2)) * t + ((float)(-2 * (1 - 2 * G2) * (1 - 2 * G2)) + a0);
    float x1 = x0 - (float)(1 - 2 * G2);
    float y1 = y0 - (float)(1 - 2 * G2);
    value += (a1 * a1) * (a1 * a1) * _fnlGradCoord2D(seed, i1, j1, x1, y1);

    int di2 = ~(aMask | cMask) | 1;
    int ndj2 = (aMask & bMask) << 1;
    float t2 = (di2 - ndj2) * (float)G2;
    float x2 = x0 - di2 + t2;
    float y2 = y0 + ndj2 + t2;
    float a2 = (2.0f / 3.0f) - x2 * x2 - y2 * y2;
    if (a2 > 0)
    {
        value += (a2 * a2) * (a2 * a2) * _fnlGradCoord2D(seed, i1 + (di2 & (-PRIME_X << 1)), j + (ndj2 & (PRIME_Y << 1)), x2, y2);
    }

    int ndi3 = (aMask & cMask) << 1;
    int dj3 = ~(aMask | bMask) | 1;
    float t3 = (dj3 - ndi3) * (float)G2;
    float x3 = x0 + ndi3 + t3;
    float y3 = y0 - dj3 + t3;
    float a3 = (2.0f / 3.0f) - x3 * x3 - y3 * y3;
    if (a3 > 0)
    {
        value += (a3 * a3) * (a3 * a3) * _fnlGradCoord2D(seed, i + (ndi3 & (PRIME_X << 1)), j1 + (dj3 & (-PRIME_Y << 1)), x3, y3);
    }

    return value * 18.24196194486065f;
}

// Cellular Noise

static float _fnlSingleCellular2D(fnl_state state, int seed, FNLfloat x, FNLfloat y)
{
    int xr = _fnlFastRound(x);
    int yr = _fnlFastRound(y);

    float distance0 = 1e10f;
    float distance1 = 1e10f;
    int closestHash = 0;

    float cellularJitter = 0.43701595f * state.cellular_jitter_mod;

    int xPrimed = (xr - 1) * PRIME_X;
    int yPrimedBase = (yr - 1) * PRIME_Y;

    FNL_FLATTEN switch (state.cellular_distance_func)
    {
    default:
    case FNL_CELLULAR_DISTANCE_EUCLIDEAN:
    case FNL_CELLULAR_DISTANCE_EUCLIDEANSQ:
    {
        for (int xi = xr - 1; xi <= xr + 1; xi++)
        {
            int yPrimed = yPrimedBase;

            for (int yi = yr - 1; yi <= yr + 1; yi++)
            {
                int hash = _fnlHash2D(seed, xPrimed, yPrimed);
                int idx = hash & (255 << 1);

                float vecX = (float)(xi - x) + RAND_VECS_2D[idx] * cellularJitter;
                float vecY = (float)(yi - y) + RAND_VECS_2D[idx | 1] * cellularJitter;

                float newDistance = vecX * vecX + vecY * vecY;

                distance1 = _fnlFastMax(_fnlFastMin(distance1, newDistance), distance0);
                if (newDistance < distance0)
                {
                    distance0 = newDistance;
                    closestHash = hash;
                }
                yPrimed += PRIME_Y;
            }
            xPrimed += PRIME_X;
        }
        break;
    }
    case FNL_CELLULAR_DISTANCE_MANHATTAN:
    {
        for (int xi = xr - 1; xi <= xr + 1; xi++)
        {
            int yPrimed = yPrimedBase;

            for (int yi = yr - 1; yi <= yr + 1; yi++)
            {
                int hash = _fnlHash2D(seed, xPrimed, yPrimed);
                int idx = hash & (255 << 1);

                float vecX = (float)(xi - x) + RAND_VECS_2D[idx] * cellularJitter;
                float vecY = (float)(yi - y) + RAND_VECS_2D[idx | 1] * cellularJitter;

                float newDistance = _fnlFastAbs(vecX) + _fnlFastAbs(vecY);

                distance1 = _fnlFastMax(_fnlFastMin(distance1, newDistance), distance0);
                if (newDistance < distance0)
                {
                    distance0 = newDistance;
                    closestHash = hash;
                }
                yPrimed += PRIME_Y;
            }
            xPrimed += PRIME_X;
        }
        break;
    }
    case FNL_CELLULAR_DISTANCE_HYBRID:
    {
        for (int xi = xr - 1; xi <= xr + 1; xi++)
        {
            int yPrimed = yPrimedBase;
            for (int yi = yr - 1; yi <= yr + 1; yi++)
            {
                int hash = _fnlHash2D(seed, xPrimed, yPrimed);
                int idx = hash & (255 << 1);

                float vecX = (float)(xi - x) + RAND_VECS_2D[idx] * cellularJitter;
                float vecY = (float)(yi - y) + RAND_VECS_2D[idx | 1] * cellularJitter;

                float newDistance = (_fnlFastAbs(vecX) + _fnlFastAbs(vecY)) + (vecX * vecX + vecY * vecY);

                distance1 = _fnlFastMax(_fnlFastMin(distance1, newDistance), distance0);
                if (newDistance < distance0)
                {
                    distance0 = newDistance;
                    closestHash = hash;
                }
                yPrimed += PRIME_Y;
            }
            xPrimed += PRIME_X;
        }
        break;
    }
    }

    FNL_FLATTEN if (state.cellular_distance_func == FNL_CELLULAR_DISTANCE_EUCLIDEAN && state.cellular_return_type >= FNL_CELLULAR_RETURN_TYPE_DISTANCE)
    {
        distance0 = _fnlFastSqrt(distance0);
        if (state.cellular_return_type >= FNL_CELLULAR_RETURN_TYPE_DISTANCE2)
            distance1 = _fnlFastSqrt(distance1);
    }

    switch (state.cellular_return_type)
    {
    case FNL_CELLULAR_RETURN_TYPE_CELLVALUE:
        return closestHash * (1 / 2147483648.0f);
    case FNL_CELLULAR_RETURN_TYPE_DISTANCE:
        return distance0 - 1;
    case FNL_CELLULAR_RETURN_TYPE_DISTANCE2:
        return distance1 - 1;
    case FNL_CELLULAR_RETURN_TYPE_DISTANCE2ADD:
        return (distance1 + distance0) * 0.5f - 1;
    case FNL_CELLULAR_RETURN_TYPE_DISTANCE2SUB:
        return distance1 - distance0 - 1;
    case FNL_CELLULAR_RETURN_TYPE_DISTANCE2MUL:
        return distance1 * distance0 * 0.5f - 1;
    case FNL_CELLULAR_RETURN_TYPE_DISTANCE2DIV:
        return distance0 / distance1 - 1;
    default:
        return 0;
    }
}

// Perlin Noise

static float _fnlSinglePerlin2D(int seed, FNLfloat x, FNLfloat y)
{
    int x0 = _fnlFastFloor(x);
    int y0 = _fnlFastFloor(y);

    float xd0 = (float)(x - x0);
    float yd0 = (float)(y - y0);
    float xd1 = xd0 - 1;
    float yd1 = yd0 - 1;

    float xs = _fnlInterpQuintic(xd0);
    float ys = _fnlInterpQuintic(yd0);

    x0 *= PRIME_X;
    y0 *= PRIME_Y;
    int x1 = x0 + PRIME_X;
    int y1 = y0 + PRIME_Y;

    float xf0 = _fnlLerp(_fnlGradCoord2D(seed, x0, y0, xd0, yd0), _fnlGradCoord2D(seed, x1, y0, xd1, yd0), xs);
    float xf1 = _fnlLerp(_fnlGradCoord2D(seed, x0, y1, xd0, yd1), _fnlGradCoord2D(seed, x1, y1, xd1, yd1), xs);

    return _fnlLerp(xf0, xf1, ys) * 1.4247691104677813f;
}

// Value Cubic

static float _fnlSingleValueCubic2D(int seed, FNLfloat x, FNLfloat y)
{
    int x1 = _fnlFastFloor(x);
    int y1 = _fnlFastFloor(y);

    float xs = x - (float)x1;
    float ys = y - (float)y1;

    x1 *= PRIME_X;
    y1 *= PRIME_Y;

    int x0 = x1 - PRIME_X;
    int y0 = y1 - PRIME_Y;
    int x2 = x1 + PRIME_X;
    int y2 = y1 + PRIME_Y;
    int x3 = x1 + PRIME_X * 2;
    int y3 = y1 + PRIME_Y * 2;

    return _fnlCubicLerp(
        _fnlCubicLerp(_fnlValCoord2D(seed, x0, y0), _fnlValCoord2D(seed, x1, y0), _fnlValCoord2D(seed, x2, y0), _fnlValCoord2D(seed, x3, y0),
                      xs),
        _fnlCubicLerp(_fnlValCoord2D(seed, x0, y1), _fnlValCoord2D(seed, x1, y1), _fnlValCoord2D(seed, x2, y1), _fnlValCoord2D(seed, x3, y1),
                      xs),
        _fnlCubicLerp(_fnlValCoord2D(seed, x0, y2), _fnlValCoord2D(seed, x1, y2), _fnlValCoord2D(seed, x2, y2), _fnlValCoord2D(seed, x3, y2),
                      xs),
        _fnlCubicLerp(_fnlValCoord2D(seed, x0, y3), _fnlValCoord2D(seed, x1, y3), _fnlValCoord2D(seed, x2, y3), _fnlValCoord2D(seed, x3, y3),
                      xs),
        ys) * (1 / (1.5f * 1.5f));
}

// Value noise

static float _fnlSingleValue2D(int seed, FNLfloat x, FNLfloat y)
{
    int x0 = _fnlFastFloor(x);
    int y0 = _fnlFastFloor(y);

    float xs = _fnlInterpHermite((float)(x - x0));
    float ys = _fnlInterpHermite((float)(y - y0));

    x0 *= PRIME_X;
    y0 *= PRIME_Y;
    int x1 = x0 + PRIME_X;
    int y1 = y0 + PRIME_Y;

    float xf0 = _fnlLerp(_fnlValCoord2D(seed, x0, y0), _fnlValCoord2D(seed, x1, y0), xs);
    float xf1 = _fnlLerp(_fnlValCoord2D(seed, x0, y1), _fnlValCoord2D(seed, x1, y1), xs);

    return _fnlLerp(xf0, xf1, ys);
}

// Domain Warp

// Forward declare
static void _fnlSingleDomainWarpBasicGrid2D(int seed, float warpAmp, float frequency, FNLfloat x, FNLfloat y, inout FNLfloat xp, inout FNLfloat yp);
static void _fnlSingleDomainWarpSimplexGradient(int seed, float warpAmp, float frequency, FNLfloat x, FNLfloat y, inout FNLfloat xr, inout FNLfloat yr, bool outGradOnly);

static void _fnlDoSingleDomainWarp2D(fnl_state state, int seed, float amp, float freq, FNLfloat x, FNLfloat y, inout FNLfloat xp, inout FNLfloat yp)
{
    FNL_FLATTEN switch (state.domain_warp_type)
    {
#if FNL_USES(FNL_USED_WARP, FNL_DOMAIN_WARP_OPENSIMPLEX2)
    case FNL_DOMAIN_WARP_OPENSIMPLEX2:
        _fnlSingleDomainWarpSimplexGradient(seed, amp * 38.283687591552734375f, freq, x, y, xp, yp, false);
        break;
#endif
#if FNL_USES(FNL_USED_WARP, FNL_DOMAIN_WARP_OPENSIMPLEX2_REDUCED)
    case FNL_DOMAIN_WARP_OPENSIMPLEX2_REDUCED:
        _fnlSingleDomainWarpSimplexGradient(seed, amp * 16.0f, freq, x, y, xp, yp, true);
        break;
#endif
#if FNL_USES(FNL_USED_WARP, FNL_DOMAIN_WARP_BASICGRID)
    case FNL_DOMAIN_WARP_BASICGRID:
        _fnlSingleDomainWarpBasicGrid2D(seed, amp, freq, x, y, xp, yp);
        break;
#endif
    default:
        break;
    }
}

// Domain Warp Single Wrapper

static void _fnlDomainWarpSingle2D(fnl_state state, inout FNLfloat x, inout FNLfloat y)
{
    int seed = state.seed;
    float amp = state.domain_warp_amp * _fnlCalculateFractalBounding(state);
    float freq = state.frequency;

    FNLfloat xs = x;
    FNLfloat ys = y;
    _fnlTransformDomainWarpCoordinate2D(state, xs, ys);

    _fnlDoSingleDomainWarp2D(state, seed, amp, freq, xs, ys, x, y);
}

// Domain Warp Fractal Progressive

static void _fnlDomainWarpFractalProgressive2D(fnl_state state, inout FNLfloat x, inout FNLfloat y)
{
    int seed = state.seed;
    float amp = state.domain_warp_amp * _fnlCalculateFractalBounding(state);
    float freq = state.frequency;

    for (int i = 0; i < state.octaves; i++)
    {
        FNLfloat xs = x;
        FNLfloat ys = y;
        _fnlTransformDomainWarpCoordinate2D(state, xs, ys);

        _fnlDoSingleDomainWarp2D(state, seed, amp, freq, xs, ys, x, y);

        seed++;
        amp *= state.gain;
        freq *= state.lacunarity;
    }
}

// Domain Warp Fractal Independent

static void _fnlDomainWarpFractalIndependent2D(fnl_state state, inout FNLfloat x, inout FNLfloat y)
{
    FNLfloat xs = x;
    FNLfloat ys = y;
    _fnlTransformDomainWarpCoordinate2D(state, xs, ys);

    int seed = state.seed;
    float amp = state.domain_warp_amp * _fnlCalculateFractalBounding(state);
    float freq = state.frequency;

    for (int i = 0; i < state.octaves; i++)
    {
        _fnlDoSingleDomainWarp2D(state, seed, amp, freq, xs, ys, x, y);

        seed++;
        amp *= state.gain;
        freq *= state.lacunarity;
    }
}

// Domain Warp Basic Grid

static void _fnlSingleDomainWarpBasicGrid2D(int seed, float warpAmp, float frequency, FNLfloat x, FNLfloat y, inout FNLfloat xp, inout FNLfloat yp)
{
    FNLfloat xf = x * frequency;
    FNLfloat yf = y * frequency;

    int x0 = _fnlFastFloor(xf);
    int y0 = _fnlFastFloor(yf);

    float xs = _fnlInterpHermite((float)(xf - x0));
    float ys = _fnlInterpHermite((float)(yf - y0));

    x0 *= PRIME_X;
    y0 *= PRIME_Y;
    int x1 = x0 + PRIME_X;
    int y1 = y0 + PRIME_Y;

    int idx0 = _fnlHash2D(seed, x0, y0) & (255 << 1);
    int idx1 = _fnlHash2D(seed, x1, y0) & (255 << 1);

    float lx0x = _fnlLerp(RAND_VECS_2D[idx0], RAND_VECS_2D[idx1], xs);
    float ly0x = _fnlLerp(RAND_VECS_2D[idx0 | 1], RAND_VECS_2D[idx1 | 1], xs);

    idx0 = _fnlHash2D(seed, x0, y1) & (255 << 1);
    idx1 = _fnlHash2D(seed, x1, y1) & (255 << 1);

    float lx1x = _fnlLerp(RAND_VECS_2D[idx0], RAND_VECS_2D[idx1], xs);
    float ly1x = _fnlLerp(RAND_VECS_2D[idx0 | 1], RAND_VECS_2D[idx1 | 1], xs);

    xp += _fnlLerp(lx0x, lx1x, ys) * warpAmp;
    yp += _fnlLerp(ly0x, ly1x, ys) * warpAmp;
}

// Domain Warp Simplex/OpenSimplex2

static void _fnlSingleDomainWarpSimplexGradient(int seed, float warpAmp, float frequency, FNLfloat x, FNLfloat y, inout FNLfloat xr, inout FNLfloat yr, bool outGradOnly)
{
    const float SQRT3 = 1.7320508075688772935274463415059f;
    const float G2 = (3 - SQRT3) / 6;

    x *= frequency;
    y *= frequency;

    /*
     * --- Skew moved to TransformNoiseCoordinate method ---
     * const FNLfloat F2 = 0.5f * (SQRT3 - 1);
     * FNLfloat s = (x + y) * F2;
     * x += s; y += s;
     */

    int i = _fnlFastFloor(x);
    int j = _fnlFastFloor(y);
    float xi = (float)(x - i);
    float yi = (float)(y - j);

    float t = (xi + yi) * G2;
    float x0 = (float)(xi - t);
    float y0 = (float)(yi - t);

    i *= PRIME_X;
    j *= PRIME_Y;

    float vx, vy;
    vx = vy = 0;

    float a = 0.5f - x0 * x0 - y0 * y0;
    if (a > 0)
    {
        float aaaa = (a * a) * (a * a);
        float xo, yo;
        if (outGradOnly)
            _fnlGradCoordOut2D(seed, i, j, xo, yo);
        else
            _fnlGradCoordDual2D(seed, i, j, x0, y0, xo, yo);
        vx += aaaa * xo;
        vy += aaaa * yo;
    }

    float c = (float)(2 * (1 - 2 * G2) * (1 / G2 - 2)) * t + ((float)(-2 * (1 - 2 * G2) * (1 - 2 * G2)) + a);
    if (c > 0)
    {
        float x2 = x0 + (2 * (float)G2 - 1);
        float y2 = y0 + (2 * (float)G2 - 1);
        float cccc = (c * c) * (c * c);
        float xo, yo;
        if (outGradOnly)
            _fnlGradCoordOut2D(seed, i + PRIME_X, j + PRIME_Y, xo, yo);
        else
            _fnlGradCoordDual2D(seed, i + PRIME_X, j + PRIME_Y, x2, y2, xo, yo);
        vx += cccc * xo;
        vy += cccc * yo;
    }

    if (y0 > x0)
    {
        float x1 = x0 + (float)G2;
        float y1 = y0 + ((float)G2 - 1);
        float b = 0.5f - x1 * x1 - y1 * y1;
        if (b > 0)
        {
            float bbbb = (b * b) * (b * b);
            float xo, yo;
            if (outGradOnly)
                _fnlGradCoordOut2D(seed, i, j + PRIME_Y, xo, yo);
            else
                _fnlGradCoordDual2D(seed, i, j + PRIME_Y, x1, y1, xo, yo);
            vx += bbbb * xo;
            vy += bbbb * yo;
        }
    }
    else
    {
        float x1 = x0 + ((float)G2 - 1);
        float y1 = y0 + (float)G2;
        float b = 0.5f - x1 * x1 - y1 * y1;
        if (b > 0)
        {
            float bbbb = (b * b) * (b * b);
            float xo, yo;
            if (outGradOnly)
                _fnlGradCoordOut2D(seed, i + PRIME_X, j, xo, yo);
            else
                _fnlGradCoordDual2D(seed, i + PRIME_X, j, x1, y1, xo, yo);
            vx += bbbb * xo;
            vy += bbbb * yo;
        }
    }

    xr += vx * warpAmp;
    yr += vy * warpAmp;
}

// ====================
// Public API
// ====================

fnl_state fnlCreateState(int seed)
{
    fnl_state newState;
    newState.seed = seed;
    newState.frequency = 0.01f;
    newState.noise_type = FNL_NOISE_OPENSIMPLEX2;
    newState.rotation_type_3d = FNL_ROTATION_NONE;
    newState.fractal_type = FNL_FRACTAL_NONE;
    newState.octaves = 3;
    newState.lacunarity = 2.0f;
    newState.gain = 0.5f;
    newState.weighted_strength = 0.0f;
    newState.ping_pong_strength = 2.0f;
    newState.cellular_distance_func = FNL_CELLULAR_DISTANCE_EUCLIDEANSQ;
    newState.cellular_return_type = FNL_CELLULAR_RETURN_TYPE_DISTANCE;
    newState.cellular_jitter_mod = 1.0f;
    newState.domain_warp_amp = 30.0f;
    newState.domain_warp_type = FNL_DOMAIN_WARP_OPENSIMPLEX2;
    return newState;
}

float fnlGetNoise2D(fnl_state state, FNLfloat x, FNLfloat y)
{
    _fnlTransformNoiseCoordinate2D(state, x, y);

    FNL_FLATTEN switch (state.fractal_type)
    {
    default:
#if FNL_USES(FNL_USED_FRACTAL, FNL_FRACTAL_NONE)
        return _fnlGenNoiseSingle2D(state, state.seed, x, y);
#else
        return 0;
#endif
#if FNL_USES(FNL_USED_FRACTAL, FNL_FRACTAL_FBM)
    case FNL_FRACTAL_FBM:
        return _fnlGenFractalFBM2D(state, x, y);
#endif
#if FNL_USES(FNL_USED_FRACTAL, FNL_FRACTAL_RIDGED)
    case FNL_FRACTAL_RIDGED:
        return _fnlGenFractalRidged2D(state, x, y);
#endif
#if FNL_USES(FNL_USED_FRACTAL, FNL_FRACTAL_PINGPONG)
    case FNL_FRACTAL_PINGPONG:
        return _fnlGenFractalPingPong2D(state, x, y);
#endif
    }
}

void fnlDomainWarp2D(fnl_state state, inout FNLfloat x, inout FNLfloat y)
{
    FNL_FLATTEN switch (state.fractal_type)
    {
    default:
#if FNL_USES(FNL_USED_FRACTAL, FNL_FRACTAL_NONE)
        _fnlDomainWarpSingle2D(state, x, y);
#endif
        break;
#if FNL_USES(FNL_USED_FRACTAL, FNL_FRACTAL_DOMAIN_WARP_PROGRESSIVE)
    case FNL_FRACTAL_DOMAIN_WARP_PROGRESSIVE:
        _fnlDomainWarpFractalProgressive2D(state, x, y);
        break;
#endif
#if FNL_USES(FNL_USED_FRACTAL, FNL_FRACTAL_DOMAIN_WARP_INDEPENDENT)
    case FNL_FRACTAL_DOMAIN_WARP_INDEPENDENT:
        _fnlDomainWarpFractalIndependent2D(state, x, y);
        break;
#endif
    }
}

