/* Procedural LCD pixel animation. Each scene is drawn at 128x112, then scaled
 * with nearest-neighbour pixels. No image decoding or per-frame allocation. */
#define PET_SCENE_W 128
#define PET_SCENE_H 112
#define PET_SCENE_SCALE 3
static uint16_t pet_scene_buf[PET_SCENE_W * PET_SCENE_H * PET_SCENE_SCALE * PET_SCENE_SCALE];
static lv_obj_t *pet_care_canvas, *pet_care_progress;
static lv_timer_t *pet_scene_timer;
static uint32_t pet_care_started, pet_care_duration;
static uint16_t pet_ink[4];

/* Every pixel canvas in the pet app draws through one target: care scenes,
 * the living habitat, meters, icons and the arcade mini-games. */
static uint16_t *pet_tgt = pet_scene_buf;
static int pet_tgt_w = PET_SCENE_W, pet_tgt_h = PET_SCENE_H, pet_tgt_s = PET_SCENE_SCALE;

static void pet_target(uint16_t *buffer, int w, int h, int scale)
{
    pet_tgt = buffer; pet_tgt_w = w; pet_tgt_h = h; pet_tgt_s = scale;
}

static void pet_inks_init(void)
{
    pet_ink[0]=lv_color_to_u16(lv_color_hex(PET_LCD));
    pet_ink[1]=lv_color_to_u16(lv_color_hex(PET_DARK));
    pet_ink[2]=lv_color_to_u16(lv_color_hex(PET_MID));
    pet_ink[3]=lv_color_to_u16(lv_color_hex(PET_LIGHT));
}

static void pet_px(int x, int y, int w, int h, int ink)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > pet_tgt_w ? pet_tgt_w : x + w;
    int y1 = y + h > pet_tgt_h ? pet_tgt_h : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    int s = pet_tgt_s, stride = pet_tgt_w * s;
    uint16_t color = pet_ink[ink & 3];
    for (int yy = y0 * s; yy < y1 * s; yy++)
    {
        uint16_t *row = pet_tgt + yy * stride;
        for (int xx = x0 * s; xx < x1 * s; xx++) row[xx] = color;
    }
}

static void pet_line(int x, int y, int xx, int yy, int ink)
{
    int dx = abs(xx-x), sx = x<xx ? 1 : -1;
    int dy = -abs(yy-y), sy = y<yy ? 1 : -1, err = dx+dy;
    for (;;) {
        pet_px(x,y,2,2,ink);
        if (x==xx && y==yy) break;
        int e = 2*err;
        if (e>=dy) { err+=dy; x+=sx; }
        if (e<=dx) { err+=dx; y+=sy; }
    }
}

static void pet_disc(int cx, int cy, int r, int ink)
{
    for (int y=-r; y<=r; y++) for (int x=-r; x<=r; x++)
        if (x*x+y*y<=r*r) pet_px(cx+x,cy+y,1,1,ink);
}

static void pet_spark(int x, int y, int r)
{
    pet_px(x-r,y,r*2+1,1,1); pet_px(x,y-r,1,r*2+1,1);
}

/* Body pose is articulated independently of scenery: leaning head, stepping
 * feet, reaching paws, closed eyelids, chewing jaw and species-specific tail.
 * `look` slides the eyes and mouth sideways (a head turn or a gaze), `happy`
 * draws closed smiling eyes with blush, and eggs wobble without limbs. */
static void pet_hab_ghost(int x, int y, unsigned t);
static void pet_actor_ex(int x, int y, enum pet_motion motion, unsigned f,
                         int look, enum sl_pet_stage stage, bool happy)
{
    if (pet.dead && stage != SL_PET_EGG) { pet_hab_ghost(x, y - 8, f * 2); return; }
    bool asleep = motion == PET_SLEEPING || motion == PET_HOSPITAL || motion == PET_HUNGOVER;
    bool dance = motion == PET_CLUBBING || motion == PET_MANIC || motion == PET_CELEBRATE;
    int bob = dance ? (int)(f%4)-2 : motion == PET_CALMING ? (int)(f/3%3)-1 : (int)(f/2%2);
    int lean = dance ? ((f/2%2) ? 2 : -2) : motion == PET_FEEDING ? (f%4<2 ? 2 : 0) : 0;
    if (stage == SL_PET_EGG) { bob = 0; lean = motion == PET_IDLE ? 0 : (f%4<2 ? 1 : -1); }
    if (look && !dance && motion != PET_FEEDING) lean = look > 0 ? 1 : -1;
    y += bob;
    const char *const *rows = pet_pattern(pet.species, stage);
    int eye_top = -1, eye_left = 16, eye_right = -1;
    for (int yy=0; yy<16; yy++) for (int xx=0; xx<16; xx++) if (rows[yy][xx]=='e') {
        if (eye_top < 0) eye_top = yy;
        if (xx < eye_left) eye_left = xx;
        if (xx > eye_right) eye_right = xx;
    }
    /* Pass 1: body, with face features painted as skin so they can move. */
    for (int yy=0; yy<16; yy++) for (int xx=0; xx<16; xx++) {
        char c=rows[yy][xx];
        if (c=='.') continue;
        int ink=c=='o'||c=='e'||c=='m'?2:c=='s'?3:1;
        pet_px(x+xx*2+(yy<10?lean:0),y+yy*2,2,2,ink);
    }
    /* Pass 2: eyes and mouth, shifted by the gaze. */
    for (int yy=0; yy<16; yy++) for (int xx=0; xx<16; xx++) {
        char c=rows[yy][xx];
        if (c!='e' && c!='m') continue;
        int sx=xx+look;
        if (sx<0 || sx>15 || rows[yy][sx]=='.' || rows[yy][sx]=='#') sx=xx;
        int px=x+sx*2+(yy<10?lean:0), py=y+yy*2;
        if (c=='m') { pet_px(px,py,2,2,motion==PET_FEEDING && f%4<2 ? 3 : 1); continue; }
        if (happy) {
            if (yy==eye_top) pet_px(px,py+1,2,1,1);
            else {
                if (xx==0 || rows[yy][xx-1]!='e') pet_px(px,py,1,1,1);
                if (xx==15 || rows[yy][xx+1]!='e') pet_px(px+1,py,1,1,1);
            }
        } else if (asleep || (motion==PET_CALMING && f%6<4)) {
            if (yy!=eye_top) pet_px(px,py,2,1,1);
        } else pet_px(px,py,2,2,1);
    }
    if (happy && eye_top >= 0) {
        pet_px(x+(eye_left-1)*2+lean,y+eye_top*2+5,2,1,3);
        pet_px(x+(eye_right+1)*2+lean,y+eye_top*2+5,2,1,3);
    }
    if (stage == SL_PET_EGG) return;
    if (pet.species==SL_PET_SLOP) {
        /* The tail wags faster when the puppy is pleased. */
        int wag = happy || dance ? f%2 : f/3%2;
        pet_line(x+27,y+26,x+36,y+(wag?20:25),1);
        /* The puppy's dark muzzle opens into a visible chewing mouth. */
        if (motion==PET_FEEDING) pet_px(x+14+lean,y+21,8,f%4<2?5:2,3);
    } else if (pet.species==SL_PET_SPAGHET) {
        pet_line(x+10,y+28,x+8+(f%2?3:-2),y+36,3);
        pet_line(x+22,y+28,x+24+(f%2?-2:3),y+36,3);
    } else if (pet.species==SL_PET_BUN) {
        if (f%4<2) { pet_px(x+4,y-2,4,3,1); pet_px(x+24,y-2,4,3,1); }
    } else if (pet.species==SL_PET_BOT) {
        if (f%2) pet_disc(x+15,y-3,2,3);
    }
    int paw = dance ? (f%2?8:16) : motion==PET_FEEDING ? 19 : motion==PET_WASHING ? 9+(f%3)*3 : 25;
    pet_line(x+2,y+24,x-5,y+paw,1);
    pet_line(x+29,y+24,x+35,y+(dance?24-paw:paw),1);
    if (!asleep) {
        pet_px(x+5,y+30,7,3,1); pet_px(x+22,y+30,7,3,1);
        if (dance || motion==PET_PLAYING) pet_px(x+(f%2?4:22),y+32,8,2,1);
    }
}

static void pet_actor(int x, int y, enum pet_motion motion, unsigned f)
{
    pet_actor_ex(x, y, motion, f, 0, SL_PET_GROWN, false);
}

/* Glyph strings: '#' dark, 'o' mid, '+' light, anything else transparent.
 * mode 1 keeps only the outline (empty hearts); mode 2 is a lit inverse. */
static void pet_glyph_s(const char *const *rows, int n, int x, int y, int mode, int scale)
{
    for (int yy=0; yy<n; yy++) for (int xx=0; rows[yy][xx]; xx++) {
        char c=rows[yy][xx];
        int ink=c=='#'?1:c=='o'?2:c=='+'?3:-1;
        if (ink<0) continue;
        if (mode==1 && ink!=1) continue;
        if (mode==2) ink=ink==1?3:ink==3?0:3;
        pet_px(x+xx*scale,y+yy*scale,scale,scale,ink);
    }
}

static void pet_glyph(const char *const *rows, int n, int x, int y, int mode)
{
    pet_glyph_s(rows, n, x, y, mode, 1);
}

/* Fifteen recognizable silhouettes, each with its own details, all in the
 * same four-tone LCD palette. Bite masks progressively remove the food. */
static void pet_food_draw(int kind, int x, int y, int bites)
{
    switch (kind) {
    case 0: /* pizza slice: thick crust, cheese triangle and pepperoni */
        for(int i=0;i<18;i++) pet_px(x+i,y+i/2,2,20-i,2);
        pet_line(x,y,x,y+20,1);pet_line(x,y,x+20,y+10,1);pet_line(x,y+20,x+20,y+10,1);
        pet_px(x-2,y,4,21,1);pet_disc(x+5,y+7,2,1);pet_disc(x+11,y+11,2,1);pet_disc(x+4,y+15,2,1);break;
    case 1: /* layered cake and cherry */
        pet_px(x,y+5,22,17,1);pet_px(x+2,y+8,18,4,3);pet_px(x+2,y+16,18,3,3);
        pet_line(x,y+5,x+17,y,1);pet_line(x+17,y,x+22,y+5,1);pet_disc(x+13,y+2,3,1);break;
    case 2: pet_disc(x+7,y+13,8,1);pet_disc(x+15,y+13,8,1);pet_disc(x+7,y+11,5,2);
        pet_line(x+11,y+6,x+13,y,1);pet_px(x+14,y,7,3,2);break;
    case 3: for(int i=0;i<18;i++)pet_px(x+i/2,y+i,12-i/2,1,1);
        pet_line(x+5,y,x+1,y-6,2);pet_line(x+7,y,x+11,y-7,2);
        pet_px(x+3,y+5,5,2,3);pet_px(x+6,y+11,3,2,3);break;
    case 4: pet_px(x,y+5,23,14,1);pet_px(x+2,y+3,19,8,3);pet_px(x+9,y+3,5,16,2);
        pet_px(x+2,y+15,5,2,3);break;
    case 5: pet_disc(x+11,y+14,12,1);pet_disc(x+11,y+13,9,3);
        for(int i=0;i<5;i++)pet_line(x+4,y+7+i*3,x+18,y+10+i*2,2);
        pet_disc(x+11,y+12,3,1);break;
    case 6: pet_disc(x+11,y+10,11,1);pet_px(x,y+10,23,13,1);
        pet_px(x+2,y+11,19,3,3);pet_px(x+2,y+17,19,3,2);
        for(int i=0;i<3;i++)pet_px(x+5+i*5,y+5,2,2,3);break;
    case 7: pet_disc(x+11,y+11,11,1);pet_disc(x+11,y+11,9,2);
        for(int i=0;i<5;i++)pet_disc(x+4+(i*7)%15,y+4+(i*5)%15,2,1);break;
    case 8: pet_disc(x+11,y+10,11,1);pet_disc(x+7,y+5,10,0);
        pet_line(x+5,y+18,x+19,y+13,3);pet_px(x+19,y+3,3,5,1);break;
    case 9: pet_disc(x+11,y+12,12,1);pet_px(x-2,y-1,26,12,0);
        pet_line(x,y+10,x+23,y+10,2);for(int i=0;i<5;i++)pet_disc(x+2+i*4,y+10,2,i%2?3:1);break;
    case 10: pet_px(x,y+9,23,14,1);for(int i=0;i<23;i++)pet_px(x+i,y+9-i/3,1,12,2);
        pet_disc(x+6,y+13,3,1);pet_disc(x+17,y+16,2,1);pet_disc(x+15,y+8,2,1);break;
    case 11: pet_disc(x+11,y+11,12,1);pet_px(x-2,y-2,27,12,0);pet_px(x,y+9,23,3,3);
        pet_line(x+5,y+6,x+7,y+1,2);pet_line(x+14,y+5,x+16,y-1,2);break;
    case 12: pet_disc(x+11,y+15,12,1);pet_px(x-2,y+16,27,10,0);
        for(int i=0;i<5;i++)pet_line(x+4+i*4,y+5,x+7+i*3,y+14,3);break;
    case 13: for(int i=0;i<15;i++)pet_px(x+4+i/2,y+10+i,15-i,1,1);
        pet_disc(x+11,y+7,9,1);pet_disc(x+9,y+5,6,3);break;
    case 14: pet_disc(x+11,y+15,12,1);pet_px(x-2,y-2,27,14,0);
        for(int i=0;i<4;i++)pet_line(x+4+i*4,y+11,x+6+i*4,y+2,2);
        pet_line(x+16,y-2,x+23,y+12,1);pet_line(x+20,y-3,x+26,y+10,1);break;
    }
    for(int i=0;i<bites;i++)pet_disc(x+20-i*6,y+7+i%2*8,6,0);
}

static void pet_scene_render(uint32_t elapsed)
{
    pet_inks_init();
    pet_target(pet_scene_buf,PET_SCENE_W,PET_SCENE_H,PET_SCENE_SCALE);
    pet_px(0,0,128,112,0);
    unsigned f=elapsed/100;
    unsigned progress=pet_care_duration ? (uint64_t)elapsed*1000/pet_care_duration : 0;
    if(progress>999)progress=999;
    pet_px(5,96,118,2,2);
    const char *caption=NULL;
    switch(pet_motion) {
    case PET_FEEDING: {
        int approach=progress<180 ? 24-(int)progress*24/180 : 0;
        pet_actor(32-approach,45,PET_FEEDING,f);
        pet_px(72,78,40,4,1);pet_px(78,82,3,14,1);pet_px(105,82,3,14,1);
        pet_px(76,75,30,2,3);
        int bite=progress<300?0:progress<470?1:progress<640?2:3;
        if(progress<800) {
            int lift=progress>=180 && f%8<4 ? 12 : 0;
            pet_food_draw(pet_pending_food,80-lift,51-lift/2,bite);
            pet_line(62,65,80-lift,65-lift/2,1);
            if(bite)for(int i=0;i<3;i++)pet_px(65+i*4,72+(f+i*3)%13,2,2,1);
        } else { pet_spark(25,42,3);pet_spark(76,36,4); }
        caption=progress<180?"DINNER IS SERVED":progress<800?"NOM NOM NOM":"HAPPY TUMMY!";
        break; }
    case PET_WASHING:
        /* Pet climbs in, washes with moving sponge, rinses, then towel-dries. */
        pet_px(90,14,3,28,1);pet_px(80,14,13,3,1);pet_px(76,18,12,4,1);
        pet_actor(progress<170?26+(int)progress/9:44,progress<170?48:55,PET_WASHING,f);
        if(progress<780) {
            pet_px(23,77,81,6,1);pet_px(28,83,71,13,2);pet_px(34,96,5,4,1);pet_px(88,96,5,4,1);
            for(int i=0;i<8;i++)pet_disc(31+i*9,75+(f+i)%3,4,3);
            if(progress>170 && progress<480) {
                pet_px(42+(f%5)*4,57,12,6,1);pet_px(44+(f%5)*4,56,8,4,3);
                for(int i=0;i<5;i++) {int bx=35+i*12,by=65-(f*2+i*7)%30;pet_disc(bx,by,3,2);pet_disc(bx,by,2,0);}
            }
            if(progress>=480)for(int i=0;i<5;i++)pet_line(77+i*2,25+(f*4+i*7)%35,76+i*2,30+(f*4+i*7)%35,2);
        } else {
            pet_px(42,73,39,15,3);pet_px(46+(f%3)*3,73,4,15,2);
            pet_spark(30,48,4);pet_spark(87,55,3);
        }
        caption=progress<170?"INTO THE TUB":progress<480?"SCRUB A DUB DUB":progress<780?"RINSE THE BUBBLES":"TOWEL DRY. ALL CLEAN!";break;
    case PET_SLEEPING:
    case PET_HUNGOVER:
        pet_px(19,61,4,35,1);pet_px(102,71,4,25,1);pet_px(23,84,79,8,1);
        pet_px(26,63,34,13,3);
        pet_actor(progress<200?28+(int)progress/10:48,49,PET_SLEEPING,f/2);
        pet_px(43,76+(f/5%2),59,9,2);pet_px(43,75+(f/5%2),59,2,1);
        for(int i=0;i<3;i++) {int x=76+i*9,y=45-i*10-(f/3%5);pet_line(x,y,x+5,y,1);pet_line(x+5,y,x,y+5,1);pet_line(x,y+5,x+5,y+5,1);}
        pet_disc(23,24,10,1);pet_disc(27,20,9,0);
        caption=progress<200?"GETTING COSY":"SWEET DREAMS";break;
    case PET_CALMING: {
        pet_px(32,87,64,5,2);
        bool breathe=(f/12)%2==0;
        int radius=12+(breathe?(int)(f%12):12-(int)(f%12));
        pet_disc(64,58,radius+15,2);pet_disc(64,58,radius+13,0);
        pet_actor(48,51,PET_CALMING,f/2);
        /* Cup of tea and a slow drifting leaf. */
        pet_px(94,80,12,11,1);pet_px(106,82,4,6,1);pet_px(97,81,6,2,3);
        pet_line(98,76,100,70-(f/4%4),2);pet_line(25,35+f%20,31,32+f%20,2);
        caption=breathe?"BREATHE IN...":"AND BREATHE OUT...";break; }
    case PET_HOSPITAL:
        if(progress<240) {
            /* Walking into a hospital with a large medical cross. */
            pet_px(74,24,44,72,2);pet_px(89,62,18,34,1);
            pet_px(91,30,10,25,3);pet_px(83,38,26,9,3);
            pet_actor(6+(int)progress/4,62,PET_PLAYING,f);
            caption="ARRIVING AT THE HOSPITAL";
        } else if(progress<800) {
            pet_px(16,57,4,39,1);pet_px(98,69,4,27,1);pet_px(20,85,78,7,1);
            pet_px(23,62,32,12,3);pet_actor(44,48,PET_HOSPITAL,f/3);pet_px(40,76,58,9,3);
            pet_px(104,25,2,65,1);pet_px(98,29,12,17,3);pet_line(99,47,87,68,2);
            pet_px(18,14,45,27,1);pet_px(21,17,39,21,0);
            for(int i=0;i<34;i++) {int phase=(i+f)%19;int h=phase==8?-7:phase==9?8:phase==10?-3:0;pet_px(23+i,28+h,2,2,1);}
            /* Calendar pages visibly turn while the pet recovers. */
            pet_px(73,15,17,21,1);pet_px(75,21,13,13,3);
            for(int i=0;i<(int)(f/3%6)+1;i++)pet_px(77+i%3*3,24+i/3*4,2,2,2);
            caption=progress<500?"REST, CARE AND RECOVERY":"ABOUT A MONTH LATER...";
        } else {
            pet_actor(48,58,PET_CELEBRATE,f);pet_spark(29,47,5);pet_spark(91,38,4);
            pet_px(15,18,10,23,1);pet_px(9,24,22,10,1);
            caption="FEELING HEALTHY AGAIN";
        }break;
    case PET_CLUBBING:
        pet_px(63,0,2,15,1);pet_disc(64,23,11,1);
        for(int i=0;i<6;i++)pet_px(56+i%3*6,16+i/3*8,3,4,(i+f)%2?3:2);
        pet_line(54,29,8+(f%6)*3,81,2);pet_line(74,29,118-(f%6)*3,81,2);
        for(int i=0;i<2;i++) {int x=i?104:7;pet_px(x,60,18,35,1);pet_disc(x+9,70,5,2);pet_disc(x+9,85,7,2);pet_disc(x+9,85,3,1);}
        for(int i=0;i<6;i++)pet_line(64,82,i*25,111,2);
        pet_px(0,101,128,1,2);pet_px(0,110,128,1,2);
        pet_actor(48+(f%6<3?-3:3),55-(f%3)*2,PET_CLUBBING,f);
        for(int i=0;i<3;i++) {int x=20+i*39,y=25+(f*2+i*9)%22;pet_disc(x,y,3,1);pet_px(x+2,y-10,2,10,1);pet_px(x+2,y-10,6,2,1);}
        caption=progress<180?"ON THE DANCE FLOOR":progress<830?"LITTLE PAWS. BIG MOVES.":"ONE LAST DANCE!";break;
    case PET_MANIC:
        /* Brief table lean, followed by visibly escalating frantic movement. */
        if(progress<280) {
            pet_actor(34,49,PET_FEEDING,f);pet_px(75,76,37,4,1);pet_px(79,80,3,16,1);
            pet_px(89,73,13,2,3);pet_line(64,69,87,72,1);
            caption="A BAD IDEA...";
        } else {
            pet_actor(38+(f*7)%27,44+(f%4)*4,PET_MANIC,f);
            for(int i=0;i<5;i++) {int x=16+i*23,y=17+(f*3+i*13)%28;pet_line(x,y,x-4,y+8,1);pet_line(x-4,y+8,x+3,y+8,1);pet_line(x+3,y+8,x-1,y+16,1);}
            caption="OVERWHELMED. TIME FOR HELP.";
        }break;
    case PET_FAREWELL:
        /* Gentle and without drama: a last cuddle with hearts, a peaceful
         * sleep while the light fades, then its own little ghost rises over
         * a stone with a flower, halo and sparkles. */
        if(progress<330) {
            pet_px(26,84,76,8,1);pet_px(30,80,68,5,3);
            pet_actor(48,50,PET_CALMING,f/2);
            for(int i=0;i<3;i++) {
                int x=26+i*34,y=40-(int)((f*2+i*9)%26);
                pet_disc(x-2,y,2,1);pet_disc(x+2,y,2,1);pet_px(x-3,y+1,7,2,1);pet_px(x-2,y+3,5,1,1);pet_px(x-1,y+4,3,1,1);
            }
            caption="ONE LAST CUDDLE";
        } else if(progress<620) {
            pet_px(26,84,76,8,1);pet_px(30,80,68,5,3);
            pet_actor(48,52,PET_SLEEPING,f/3);
            /* the room slowly dims, line by line */
            int dim=(int)(progress-330)*56/290;
            for(int y=0;y<dim;y+=2)pet_px(0,y,128,1,2);
            for(int y=111;y>111-dim;y-=2)pet_px(0,y,128,1,2);
            caption="RESTING PEACEFULLY";
        } else {
            int rise=(int)(progress-620);
            int stone=rise<120?rise*28/120:28;
            pet_px(38,93,52,3,1);
            pet_px(52,93-stone,24,stone,1);
            if(stone>4){pet_px(54,95-stone,20,stone-4,2);pet_px(55,93-stone,18,2,1);}
            if(stone>=28){pet_px(63,70,2,12,1);pet_px(59,74,10,2,1);pet_disc(86,89,2,3);pet_px(86,91,1,3,1);}
            if(rise>120) {
                int up=rise>380?(380-120)*44/260:(rise-120)*44/260;
                pet_hab_ghost(48,52-up,f);
                pet_spark(30,30+(int)(f%20),2);pet_spark(98,24+(int)((f+7)%20),2);
            }
            caption=rise<380?"GOODBYE, LITTLE FRIEND":"ALWAYS REMEMBERED";
        }
        break;
    case PET_POKE: {
        /* Its little body lies belly-up, feet in the air, eyes crossed out. A
         * stick pokes it three times; it wobbles and does nothing. Its
         * ghost pops up to watch, and sighs. */
        pet_px(0,86,128,26,2);
        for(int i=0;i<16;i++)pet_px(4+i*8,84-(i%3),2,3,1);
        int cycle=(int)(progress%250);                 /* three pokes, 250 each */
        bool touch=cycle>=110 && cycle<170 && progress<750;
        int jab=cycle<110?cycle*22/110:cycle<170?22:22-(cycle-170)*22/80;
        if(progress>=750)jab=0;
        int wob=touch?((f&1)?2:-1):0;
        const char *const *rows=pet_pattern(pet.species,SL_PET_GROWN);
        int bx=22,by=38+wob,ex0[2]={99,99},ey0[2]={99,99},ex1[2]={-1,-1},ey1[2]={-1,-1};
        for(int yy=0;yy<16;yy++)for(int xx=0;xx<16;xx++){
            char c=rows[yy][xx];
            if(c=='.')continue;
            int px=bx+xx*3,py=by+(15-yy)*3;             /* upside down: feet in the air */
            if(c=='e'){int k=xx<8?0:1;
                if(px<ex0[k])ex0[k]=px;if(py<ey0[k])ey0[k]=py;if(px+2>ex1[k])ex1[k]=px+2;if(py+2>ey1[k])ey1[k]=py+2;
                pet_px(px,py,3,3,2);continue;}
            pet_px(px,py,3,3,c=='#'||c=='d'||c=='m'?1:c=='o'?2:3);
        }
        for(int k=0;k<2;k++)if(ex1[k]>=0){           /* X for eyes */
            pet_line(ex0[k]-1,ey0[k]-1,ex1[k],ey1[k],1);pet_line(ex0[k]-1,ey1[k],ex1[k],ey0[k]-1,1);}
        /* the stick: from off to the right, jabbing its side */
        int tip=76-jab;
        pet_line(tip,62+wob,126,44,1);pet_line(tip+1,64+wob,126,46,1);
        pet_px(102,51,3,3,2);
        if(touch){pet_spark(tip-3,60+wob,3);pet_px(tip-9,54,2,2,1);pet_px(tip-12,66,2,2,1);}
        if(progress>=750) {
            int rise=(int)(progress-750)*20/250;
            pet_hab_ghost(46,16-rise,f);
            for(int i=0;i<3;i++)pet_disc(88+i*6,20+(int)((f+i*3)%8)/3,1,1);
        }
        caption=progress<250?"POKE...":progress<500?"POKE POKE...":progress<750?"...NOTHING.":"ITS GHOST SIGHS.";
        break; }
    case PET_CELEBRATE:
        pet_actor(48+(f%4<2?-2:2),52-(f%2)*6,PET_CELEBRATE,f);
        pet_spark(22,36,4); pet_spark(106,36,4);
        pet_spark(30,68,3); pet_spark(98,68,3);
        for(int i=0;i<4;i++) pet_disc(26+i*24,20+(f*3+i*7)%25,2,(i+f)%2?3:1);
        caption="SO HAPPY!";
        break;
    default:
        pet_actor(48,58,pet_motion,f);pet_spark(26,43,3);pet_spark(95,51,4);break;
    }
    if(pet_care_canvas)lv_obj_invalidate(pet_care_canvas);
    if(caption && pet_care_caption)lv_label_set_text(pet_care_caption,caption);
    if(pet_care_progress)lv_bar_set_value(pet_care_progress,progress,LV_ANIM_OFF);
}

/* The scene's sounds, on its own beats: bites, bubbles, the club's kick
 * drum, the clinic monitor, the stick. 'beat' counts the ones played. */
static int pet_care_beat;
static void pet_care_sound(uint32_t elapsed)
{
    unsigned progress = pet_care_duration ? (uint64_t)elapsed * 1000 / pet_care_duration : 0;
    int due;
    switch (pet_motion) {
    case PET_FEEDING:  due = progress >= 640 ? 3 : progress >= 470 ? 2 : progress >= 300 ? 1 : 0; break;
    case PET_WASHING:  due = (int)(elapsed / 260) + 1; break;
    case PET_CLUBBING: due = (int)(elapsed / 470) + 1; break;
    case PET_HOSPITAL: due = (int)(elapsed / 800) + 1; break;
    case PET_POKE:     due = progress >= 750 ? 4 : progress >= 610 ? 3 : progress >= 360 ? 2 :
                             progress >= 110 ? 1 : 0; break;
    case PET_FAREWELL: due = progress >= 560 ? 1 : 0; break;
    default: return;
    }
    if (due <= pet_care_beat) return;
    pet_care_beat = due;
    switch (pet_motion) {
    case PET_FEEDING:  sfx_play_vary(SFX_PET_CHOMP); break;
    case PET_WASHING:  sfx_play_vary(SFX_PET_BUBBLE); break;
    case PET_CLUBBING: sfx_play(SFX_PET_BEAT); break;
    case PET_HOSPITAL: sfx_play(SFX_PET_EKG); break;
    case PET_POKE:     sfx_play(due == 4 ? SFX_PET_GHOST : SFX_PET_POKE); break;
    default:           sfx_play(SFX_PET_GHOST); break;
    }
}

static void pet_scene_tick(lv_timer_t *timer)
{
    (void)timer;
    if(pet_care_locked && pet_care_canvas)
    {
        pet_scene_render(sl_ms()-pet_care_started);
        pet_care_sound(sl_ms()-pet_care_started);
    }
}

static void pet_scene_close(void)
{
    if(pet_scene_timer)lv_timer_delete(pet_scene_timer);
    pet_scene_timer=NULL;
    if(pet_care_overlay)lv_obj_delete(pet_care_overlay);
    pet_care_overlay=pet_care_caption=pet_care_canvas=pet_care_progress=NULL;
}
