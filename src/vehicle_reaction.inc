constexpr double kReactionMaxN=1500,kGravity=9.81,kFallbackCharacterKg=80;
SRWLOCK g_reaction_lock=SRWLOCK_INIT;
SwmlVehiclePushSample g_reaction_samples[16]{};
int g_reaction_n=0;
uint32_t g_reaction_seq=0;
ULONGLONG g_reaction_at=0;
double g_reaction_mass_logged=-1;
// Second form (vehicle_push2.h), for hosts that take it: what the character does, not a capped force.
constexpr size_t kObjLinVel=0x390,kObjInvMass=0x3d0;
constexpr double kMaxApproach=12,kLandMin=1.0,kMaxLand=30,kSupportMaxG=1.5;
VehiclePushSample2 g_reaction2[16]{};
int g_reaction2_n=0;
double g_prev_char_vy=0;bool g_prev_char_vy_ok=false;
double character_kg(const uint8_t* const mine[3])
{
    double kg=0;
    for(int j=0;j<3;j++)if(mine[j]){double im=rd<double>(mine[j],kObjInvMass);if(std::isfinite(im) && im>0)kg+=1/im;}
    if(!(kg>1) || kg>1000)kg=kFallbackCharacterKg;
    return kg;
}
double character_push_cap(const uint8_t* const mine[3])
{
    double kg=character_kg(mine);
    if(std::fabs(kg-g_reaction_mass_logged)>.5) {
        g_reaction_mass_logged=kg;
        swlog("large vehicle push: your character weighs %.1f kg (older hosts: pushes vehicles with at most %.0f N)",kg,std::min(kReactionMaxN,kg*kGravity));
    }
    return std::min(kReactionMaxN,kg*kGravity);
}
void sample_vehicle_reaction(const uint8_t* world,double dt,SwmlVehiclePushSample out[16],int& count,
                             VehiclePushSample2* out2=nullptr,int* count2=nullptr)
{
    count=0;
    if(count2)*count2=0;
    if(!g_is_host || *g_is_host || !(dt>0) || dt>.1)return;
    const void* own=nullptr;double pos[3];
    if(!local_player_pos(pos,&own) || !own || rd<const void*>(own,kCharSeat))return;
    const uint8_t* mine[3]{};
    for(int j=0;j<3;j++){auto so=rd<const uint8_t*>(own,kCharBodies[j]);mine[j]=so?rd<const uint8_t*>(so,kSceneRigid):nullptr;}
    auto dispatcher=rd<const uint8_t*>(world,0x28);if(!dispatcher)return;
    int nm=rd<int>(dispatcher,0x14);auto mans=rd<const uint8_t* const*>(dispatcher,0x20);
    if(nm<0 || nm>1000000 || (nm && !mans))return;
    double weights[16]{},points[16][3]{},forces[16][3]{};
    // second form, world frame, normal impulses only (friction under walking feet is no push): side contacts give the
    // horizontal impulse on the vehicle and the velocity each character body lost into it, contacts from above the
    // downward impulse and the downward speed each body lost; contact points impulse-weighted
    double imp2[16][3]{},dv2[16][3][3]{},w2[16]{},pts2[16][3]{},down2[16]{},dvd2[16][3]{};
    const uint8_t* slot_body[16]{};
    for(int i=0;i<nm;i++) {
        auto m=mans[i];auto a=rd<const uint8_t*>(m,0x5d0),b=rd<const uint8_t*>(m,0x5d8);
        bool am=a && (a==mine[0] || a==mine[1] || a==mine[2]);
        bool bm=b && (b==mine[0] || b==mine[1] || b==mine[2]);
        if(am==bm)continue;
        auto me=am?a:b;auto vehicle=am?b:a;
        if((rd<int>(me,kObjFlags)&4) || !vehicle || rd<int>(vehicle,kObjInternalType)!=2)continue;
        int flags=rd<int>(vehicle,kObjFlags);if(!(flags&0x800) || (flags&7))continue;
        auto so=rd<const uint8_t*>(vehicle,0x1f8);auto owner=so?rd<const uint8_t*>(so,0xc0):nullptr;
        if(!owner || rd<int>(owner,0)!=1)continue;
        int np=rd<int>(m,0x5e0);if(np<0 || np>4)continue;
        int slot=-1;for(int j=0;j<count;j++)if(out[j].body==vehicle)slot=j;
        if(slot<0){if(count==16)continue;slot=count++;out[slot]={};out[slot].body=vehicle;slot_body[slot]=vehicle;}
        for(int j=0;j<np;j++) {
            auto p=m+0x10+j*0x170;if(rd<double>(p,0xa0)>.02)continue;
            double normal=rd<double>(p,0xe8),lat1=rd<double>(p,0xf0),lat2=rd<double>(p,0xf8);
            if(!std::isfinite(normal) || !std::isfinite(lat1) || !std::isfinite(lat2) || normal<0)continue;
            double impulse[3],offset[3];
            for(int k=0;k<3;k++) {
                impulse[k]=(am?-1:1)*(normal*rd<double>(p,0x80+8*k)+lat1*rd<double>(p,0x130+8*k)+lat2*rd<double>(p,0x150+8*k));
                offset[k]=rd<double>(p,(am?0x40:0x60)+8*k)-rd<double>(vehicle,kObjOrigin+8*k);
            }
            {
                double ni[3];for(int k=0;k<3;k++)ni[k]=(am?-1:1)*normal*rd<double>(p,0x80+8*k); // normal impulse on the vehicle
                double total=std::sqrt(ni[0]*ni[0]+ni[1]*ni[1]+ni[2]*ni[2]);
                double im=rd<double>(me,kObjInvMass);
                int mb=me==mine[0]?0:me==mine[1]?1:2;
                if(std::isfinite(total) && total>1e-8 && std::isfinite(im) && im>0) {
                    w2[slot]+=total;
                    for(int k=0;k<3;k++)pts2[slot][k]+=offset[k]*total;
                    if(std::fabs(ni[1])<.5*total) {
                        for(int k=0;k<3;k+=2){imp2[slot][k]+=ni[k];dv2[slot][mb][k]+=ni[k]*im;}
                    } else if(ni[1]<0) {
                        down2[slot]-=ni[1];dvd2[slot][mb]-=ni[1]*im;
                    }
                }
            }
            impulse[1]=0;double weight=std::hypot(impulse[0],impulse[2]);
            if(!(weight>1e-8) || !std::isfinite(weight))continue;
            weights[slot]+=weight;
            for(int k=0;k<3;k++) {
                double lf=0,lp=0;
                for(int axis=0;axis<3;axis++) {
                    double r=rd<double>(vehicle,0x10+axis*0x20+k*8);
                    lf+=r*impulse[axis]/dt;lp+=r*offset[axis];
                }
                forces[slot][k]+=lf;points[slot][k]+=lp*weight;
            }
        }
    }
    int n=0;double total=0;
    for(int j=0;j<count;j++)if(weights[j]>0) {
        double mag=std::sqrt(forces[j][0]*forces[j][0]+forces[j][1]*forces[j][1]+forces[j][2]*forces[j][2]);
        if(!std::isfinite(mag) || mag<.01)continue;
        out[n].body=out[j].body;
        for(int k=0;k<3;k++){out[n].point[k]=float(points[j][k]/weights[j]);out[n].force[k]=float(forces[j][k]);}
        total+=mag;++n;
    }
    // second form (before the first form's slots are compacted: same slot order)
    double char_vy=0;bool char_vy_ok=false;
    for(int j=0;j<3;j++)if(mine[j]){double vy=rd<double>(mine[j],kObjLinVel+8);if(std::isfinite(vy) && (!char_vy_ok || vy<char_vy))char_vy=vy,char_vy_ok=true;}
    if(out2 && count2) {
        double kg=character_kg(mine);int n2=0;
        for(int j=0;j<count && n2<16;j++) {
            if(!(w2[j]>0))continue;
            auto vehicle=slot_body[j];if(!vehicle)continue;
            double u[3];for(int k=0;k<3;k++)u[k]=rd<double>(vehicle,kObjLinVel+8*k);
            if(!std::isfinite(u[0]) || !std::isfinite(u[1]) || !std::isfinite(u[2]))u[0]=u[1]=u[2]=0;
            double dir[3]{},approach=0;
            double h=std::hypot(imp2[j][0],imp2[j][2]);
            if(h>1e-6) {
                dir[0]=imp2[j][0]/h;dir[2]=imp2[j][2]/h;
                // the character's speed into the vehicle before the step: the most any of its bodies lost along dir,
                // plus the copy's own speed along dir (the copy only stopped it relative to itself)
                double lost=0;
                for(int b=0;b<3;b++)lost=std::max(lost,dv2[j][b][0]*dir[0]+dv2[j][b][2]*dir[2]);
                approach=std::clamp(lost+u[0]*dir[0]+u[2]*dir[2],0.0,kMaxApproach);
                if(!(lost>0))dir[0]=dir[2]=0,approach=0;
            }
            double down=down2[j],support=0,land=0;
            if(down>0) {
                support=std::min(down/dt,kg*kGravity*kSupportMaxG);
                double lost=0;for(int b=0;b<3;b++)lost=std::max(lost,dvd2[j][b]);
                // absolute downward speed: lost is relative to the copy, so add the copy's own downward speed
                double fall=std::max(lost-kGravity*dt-u[1],g_prev_char_vy_ok?-g_prev_char_vy:0.0);
                if(fall>kLandMin)land=std::min(fall,kMaxLand);
            }
            if(dir[0]==0 && dir[2]==0 && !(support>0))continue;
            VehiclePushSample2& s=out2[n2++];s={};s.body=vehicle;s.kg=float(kg);
            s.approach=float(approach);s.support=float(support);s.land=float(land);
            for(int k=0;k<3;k++) {
                double lp=0,ld=0;
                for(int axis=0;axis<3;axis++) {
                    double r=rd<double>(vehicle,0x10+axis*0x20+k*8);
                    lp+=r*pts2[j][axis]/w2[j];ld+=r*dir[axis];
                }
                s.point[k]=float(lp);s.dir[k]=float(ld);
            }
        }
        *count2=n2;
    }
    g_prev_char_vy=char_vy;g_prev_char_vy_ok=char_vy_ok;
    count=n;
    double cap=n?character_push_cap(mine):kReactionMaxN;
    if(total>cap)for(int j=0;j<count;j++)for(float& f:out[j].force)f=float(f*cap/total);
}
