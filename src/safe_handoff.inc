void handoff_current(const uint8_t* body,double tr[16]) {
    std::fill(tr,tr+16,0.0);tr[15]=1;
    for(int a=0;a<3;a++) {
        for(int b=0;b<3;b++)tr[b*4+a]=*reinterpret_cast<const double*>(body+kBodyBasis+a*0x20+b*8);
        tr[12+a]=*reinterpret_cast<const double*>(body+kBodyOrigin+a*8);
    }
}
void handoff_relative(const double from[16],const double to[16],const double part[16],double out[16]) {
    double delta[3][3]{};
    for(int a=0;a<3;a++)for(int b=0;b<3;b++)for(int k=0;k<3;k++)delta[a][b]+=to[k*4+a]*from[k*4+b];
    std::fill(out,out+16,0.0);out[15]=1;
    for(int a=0;a<3;a++) {
        out[12+a]=to[12+a];
        for(int b=0;b<3;b++) {
            out[12+a]+=delta[a][b]*(part[12+b]-from[12+b]);
            for(int k=0;k<3;k++)out[b*4+a]+=delta[a][k]*part[b*4+k];
        }
    }
}
void rebase_handoff(uint8_t* scene,Unit& u) {
    bool driven=false;for(const auto& p:u.parts)driven=driven || (p.saved_body_flags&(kFlagDriven|kFlagKinematic));
    if(!driven || !g_orig_transform || !g_orig_kin)return;
    size_t n=u.parts.size();std::vector<std::array<double,16>> current(n),target(n);
    std::vector<int> anchor(n,-1);std::vector<const uint8_t*> bodies;
    for(size_t i=0;i<n;i++) {
        auto& p=u.parts[i];handoff_current(p.body,current[i].data());target[i]=current[i];bodies.push_back(p.body);
        if(p.kind<kJointPart) {
            anchor[i]=int(i);
            if(p.replay_valid)memcpy(target[i].data(),p.replay,sizeof p.replay);
        }
    }
    int refs=0;auto links=joints_among(bodies,refs);
    for(size_t pass=0;pass<n;pass++)for(auto [a,b]:links) {
        if(anchor[a]<0 && anchor[b]>=0)anchor[a]=anchor[b];
        if(anchor[b]<0 && anchor[a]>=0)anchor[b]=anchor[a];
    }
    for(size_t i=0;i<n;i++)if(u.parts[i].kind>=kJointPart && anchor[i]>=0) {
        int root=anchor[i];handoff_relative(current[root].data(),target[root].data(),current[i].data(),target[i].data());
    }
    double maximum=0;float zero[3]{};
    for(size_t i=0;i<n;i++) {
        auto& p=u.parts[i];double d[3];for(int a=0;a<3;a++)d[a]=target[i][12+a]-current[i][12+a];maximum=std::max(maximum,length3(d));
        // Queue BOTH states in the same event batch. World pose == driven target when Bullet derives velocity.
        g_orig_transform(scene,p.so,target[i].data());g_orig_kin(scene,p.so,target[i].data());
        g_set_lin(scene,p.so,zero);g_set_ang(scene,p.so,zero);
    }
    swlog("object push: safe handoff %s %u: %zu bodies rebased together, largest correction %.3f m; direct pose and driven target agree",u.vehicle?"vehicle":"object",u.id,n,maximum);
}
