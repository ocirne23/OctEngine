vec3 quat_transform(vec3 v, vec4 q)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

vec4 quat_multiply(vec4 q, vec4 p)
{
    vec4 c, r;
    c.xyz = cross(q.xyz, p.xyz);
    c.w = -dot(q.xyz, p.xyz);
    r = p * q.w + c;
    r.xyz = (q * p.w + r).xyz;
    return r;
}

//float GetLuminance(vec3 color)
//{
//    return dot(color, vec3(0.2126, 0.7152, 0.0722));
//}
//
//// Apply sRGB gamma curve to linear values
//vec3 ToSRGB(vec3 col)
//{
//    return select(col.xyz < 0.0031308, 12.92 * col.xyz, 1.055 * pow(col.xyz, 1.0 / 2.4) - vec3(0.055, 0.055, 0.055));
//}
//
//// Inverse sRGB gamma curve to get from sRGB to linear values
//vec3 ToLinear(vec3 col)
//{
//    return select(col.xyz < 0.04045, col.xyz / 12.92, pow((col.xyz + vec3(0.055, 0.055, 0.055)) / 1.055, 2.4));
//}

//vec4 quat_multiply(vec4 a, vec4 b) {
//    return vec4(
//        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
//        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
//        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
//        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
//    );
//}
//vec4 quat_multiply(vec4 a, vec4 b) {
//    return vec4(a.w * b.xyz + b.w * a.xyz + cross(a.xyz, b.xyz), a.w * b.w - dot(a.xyz, b.xyz));
//}
//vec4 quat_invert(vec4 q) {
//    return vec4(-q.xyz, q.w);
//}
//mat3 quat_to_mat3(vec4 q)
//{
//    vec3 q2   = q.xyz + q.xyz;
//    vec3 xyz2 = q.xyz * q2;
//    vec3 wq2  = q.w * q2;
//    float xy2 = q.x * q2.y;
//    float xz2 = q.x * q2.z;
//    float yz2 = q.y * q2.z;
//
//    return mat3(
//        1.0 - (xyz2.y + xyz2.z), xy2 + wq2.z,             xz2 - wq2.y,
//        xy2 - wq2.z,             1.0 - (xyz2.x + xyz2.z), yz2 + wq2.x,
//        xz2 + wq2.y,             yz2 - wq2.x,             1.0 - (xyz2.x + xyz2.y)
//    );
//}
//
//float4 mat_to_quat(float3x3 m)
//{
//	float4 q;
//	float s,p,tr = m._m00 + m._m11 + m._m22;
//	q.w=1;q.x=0;q.y=0;q.z=0;
//	if(tr>0)
//	{
//		s=sqrt(tr+1.0f);
//		p=0.5f/s;
//		q.w=s*0.5f;
//		q.x=(m._m21-m._m12)*p;
//		q.y=(m._m02-m._m20)*p;
//		q.z=(m._m10-m._m01)*p;
//	}
//	else if ((m._m00>=m._m11) && (m._m00>=m._m22))
//	{
//		s=sqrt(m._m00-m._m11-m._m22+1.0f);
//		p=0.5f/s;
//		q.w=(m._m21-m._m12)*p;
//		q.x=s*0.5f;
//		q.y=(m._m10+m._m01)*p;
//		q.z=(m._m20+m._m02)*p;
//	}
//	else if ((m._m11>=m._m00) && (m._m11>=m._m22))
//	{
//		s=sqrt(m._m11-m._m22-m._m00+1.0f);
//		p=0.5f/s;
//		q.w=(m._m02-m._m20)*p;
//		q.x=(m._m01+m._m10)*p;
//		q.y=s*0.5f;
//		q.z=(m._m21+m._m12)*p;
//	}
//	else if ((m._m22>=m._m00) && (m._m22>=m._m11))
//	{
//		s=sqrt(m._m22-m._m00-m._m11+1.0f);
//		p=0.5f/s;
//		q.w=(m._m10-m._m01)*p;
//		q.x=(m._m02+m._m20)*p;
//		q.y=(m._m12+m._m21)*p;
//		q.z=s*0.5f;
//	}
//	return q;
//}

//T = normalize(T - dot(T, N) * N);