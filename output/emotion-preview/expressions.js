/* Dotii expression vocabulary: an original 48-state animation library. */
(() => {
  const K = (at, values = {}) => ({ at, ...values });
  const W = (channel, amp, period, phase = 0) => ({ channel, amp, period, phase });
  const FX = (...types) => types;

  const groups = {
    basic: { label: '基础状态', note: '日常存在感与对话底盘' },
    emotion: { label: '情绪表达', note: '开心、困惑、难过与亲密感' },
    dialog: { label: '对话行为', note: '聆听、处理、完成和被打断' },
    system: { label: '系统反馈', note: '连接、电量、音乐和提醒' },
  };

  const expressions = [
    {
      id: 'idle', label: '待机', name: 'IDLE', group: 'basic', accent: '#65c6ff',
      description: '放松但有生命感，视线偶尔漂移，适合长时间停留。',
      dna: ['眼睛：自然圆眼', '眉毛：放松', '嘴巴：平嘴'], sequence: ['慢速呼吸', '2–6 秒随机眨眼', '视线轻移后回正'],
      duration: 4600, base: { eyeShape: 'open', mouth: 'flat' },
      motion: [W('headY', 2.8, 4600), W('eyeGap', 0.012, 2800)],
      timeline: [K(0), K(.42, { pupilX: 0.5, headRot: 1.4 }), K(.75, { pupilX: -0.4, headRot: -1 }), K(1)]
    },
    {
      id: 'wake', label: '唤醒', name: 'WAKE', group: 'basic', accent: '#65c6ff',
      description: '从低电量般的闭合状态快速睁眼，头部弹起并主动看向前方。',
      dna: ['眼睛：闭合 → 放大', '眉毛：上挑', '嘴巴：小圆嘴'], sequence: ['眼睑快速抬起', '头部弹起', '眼神锁定', '稳定回正'],
      duration: 1700, base: { eyeShape: 'open', mouth: 'o', brow: 'raised', effects: FX('sparkle') },
      timeline: [K(0, { eyeOpen: .08, headY: 16, headScale: .97 }), K(.18, { eyeOpen: 1.35, headY: -10, headScale: 1.05 }), K(.36, { eyeOpen: 1.08, headY: 2 }), K(.62, { eyeOpen: 1, headY: 0 }), K(1)]
    },
    {
      id: 'sleep', label: '睡眠', name: 'SLEEP', group: 'basic', accent: '#8294aa',
      description: '完全收起眼神，呼吸变慢，右上角出现缓慢上升的睡眠符号。',
      dna: ['眼睛：闭眼弧线', '眉毛：下垂', '嘴巴：小嘴'], sequence: ['闭眼', '头部下沉', '慢呼吸', 'Z 符号上浮'],
      duration: 5200, base: { eyeShape: 'closed', brow: 'sad', mouth: 'small', headY: 8, effects: FX('zzz') },
      motion: [W('headY', 4, 5200), W('headScale', .012, 5200)]
    },
    {
      id: 'listening', label: '聆听', name: 'LISTENING', group: 'basic', accent: '#4bdb9f',
      description: '上眼睑打开，头部微倾，声波从左耳和右耳向内收拢。',
      dna: ['眼睛：专注圆眼', '眉毛：轻抬', '嘴巴：闭嘴'], sequence: ['头部侧听', '瞳孔对准声源', '声波出现', '保持开放'],
      duration: 2800, base: { eyeShape: 'open', eyeOpen: 1.12, brow: 'raised', browY: -3, mouth: 'small', headRot: -3, effects: FX('rings'), accent: '#4bdb9f' },
      motion: [W('headRot', 2, 3000), W('eyeX', 3, 2100)]
    },
    {
      id: 'thinking', label: '思考', name: 'THINKING', group: 'basic', accent: '#aa9bff',
      description: '视线向上方漂移，眉间轻微收紧，头顶有三颗循环轨道点。',
      dna: ['眼睛：向上看', '眉毛：内压', '嘴巴：平嘴'], sequence: ['视线上扬', '眉心收紧', '轨道点循环', '等待结论'],
      duration: 3600, base: { eyeShape: 'narrow', eyeY: -5, pupilY: -.8, brow: 'focused', mouth: 'flat', headRot: 2, effects: FX('orbit'), accent: '#aa9bff' },
      motion: [W('headRot', 2.6, 3600), W('eyeY', 2, 2200)]
    },
    {
      id: 'speaking', label: '说话', name: 'SPEAKING', group: 'basic', accent: '#4bdb9f',
      description: '嘴型按语音强度开合，头部跟着句子节拍轻点，右侧出现音量柱。',
      dna: ['眼睛：柔和圆眼', '眉毛：轻抬', '嘴巴：开合'], sequence: ['嘴型开合', '头部点动', '音量柱跳动', '句尾回落'],
      duration: 900, base: { eyeShape: 'open', brow: 'raised', mouth: 'open_smile', mouthOpen: .3, effects: FX('bars'), accent: '#4bdb9f' },
      timeline: [K(0, { mouthOpen: .18, mouthW: .96, headY: 0 }), K(.24, { mouthOpen: .72, mouthW: 1.03, headY: -3 }), K(.48, { mouthOpen: .26, mouthW: .98, headY: 1 }), K(.72, { mouthOpen: .58, mouthW: 1.02, headY: -2 }), K(1, { mouthOpen: .2 })]
    },
    {
      id: 'blink', label: '眨眼', name: 'BLINK', group: 'basic', accent: '#65c6ff',
      description: '一个快而软的眨眼，用来打断长时间凝视，不会改变当前情绪。',
      dna: ['眼睛：完整开合', '眉毛：保持', '嘴巴：保持'], sequence: ['快速闭合', '短暂停留', '弹性打开'],
      duration: 1500, base: { eyeShape: 'open', mouth: 'small' },
      timeline: [K(0, { eyeOpen: 1 }), K(.12, { eyeOpen: .05, eyeH: .32 }), K(.25, { eyeOpen: .05, eyeH: .32 }), K(.48, { eyeOpen: 1.08, eyeH: 1.03 }), K(.7, { eyeOpen: 1 }), K(1, { eyeOpen: 1 })]
    },
    {
      id: 'ready', label: '就绪', name: 'READY', group: 'basic', accent: '#4bdb9f',
      description: '眼睛比待机更亮、更稳，表示设备已经完成连接并可以接受指令。',
      dna: ['眼睛：明亮圆眼', '眉毛：平眉', '嘴巴：轻微笑意'], sequence: ['眼神对齐', '轻微点头', '保持稳定'],
      duration: 3200, base: { eyeShape: 'open', eyeOpen: 1.05, brow: 'neutral', mouth: 'smile', mouthCurve: .25, effects: FX('check'), accent: '#4bdb9f' },
      motion: [W('headY', 2, 3200)]
    },
    {
      id: 'calm', label: '平静', name: 'CALM', group: 'basic', accent: '#7095c9',
      description: '眼皮放松，嘴角带一点温度，适合不说话的陪伴状态。',
      dna: ['眼睛：半闭', '眉毛：放松', '嘴巴：小微笑'], sequence: ['呼吸下沉', '眼皮放松', '嘴角上扬'],
      duration: 4800, base: { eyeShape: 'sleepy', brow: 'neutral', mouth: 'small_smile', blush: .12, headY: 2 },
      motion: [W('headY', 3, 4800), W('blush', .04, 5200)]
    },
    {
      id: 'curious', label: '好奇', name: 'CURIOUS', group: 'basic', accent: '#f6bd4d',
      description: '一只眼睛放大、一只保持观察，头部侧倾并提出一个问题。',
      dna: ['眼睛：左大右窄', '眉毛：左挑', '嘴巴：小嘴'], sequence: ['侧头', '左侧眼睛放大', '眉峰上挑', '问题符号浮出'],
      duration: 3200, base: { leftEyeShape: 'wide', rightEyeShape: 'narrow', brow: 'asym_raised', mouth: 'small', headRot: -6, effects: FX('question'), accent: '#f6bd4d' },
      motion: [W('headRot', 2.5, 3000)]
    },
    {
      id: 'happy', label: '开心', name: 'HAPPY', group: 'emotion', accent: '#ffb84d',
      description: '眼角向上、嘴角明显上扬，腮红和轻微弹跳让笑容不是静态的。',
      dna: ['眼睛：笑眼弧', '眉毛：上挑', '嘴巴：大微笑'], sequence: ['眼睛先笑', '嘴角上扬', '身体轻弹', '稳定回正'],
      duration: 2200, base: { eyeShape: 'happy', brow: 'raised', mouth: 'smile', mouthCurve: .9, blush: .28, effects: FX('sparkle'), accent: '#ffb84d' },
      timeline: [K(0, { headY: 2, headScale: .99 }), K(.24, { headY: -8, headScale: 1.03 }), K(.5, { headY: 1, headScale: 1 }), K(1, { headY: 2, headScale: .99 })]
    },
    {
      id: 'laugh', label: '大笑', name: 'LAUGH', group: 'emotion', accent: '#ffb84d',
      description: '笑眼闭合，嘴巴张开，头部上下弹动，周围出现连续闪光。',
      dna: ['眼睛：闭合笑眼', '眉毛：高挑', '嘴巴：张嘴笑'], sequence: ['笑眼闭合', '张嘴', '连续点头', '闪光扩散'],
      duration: 1150, base: { eyeShape: 'happy', mouth: 'open_smile', mouthOpen: .45, brow: 'raised', blush: .32, effects: FX('sparkle'), accent: '#ffb84d' },
      timeline: [K(0, { headY: 0 }), K(.2, { headY: -10, mouthOpen: .78 }), K(.4, { headY: 4, mouthOpen: .35 }), K(.6, { headY: -8, mouthOpen: .7 }), K(.82, { headY: 2, mouthOpen: .3 }), K(1, { headY: 0 })]
    },
    {
      id: 'excited', label: '兴奋', name: 'EXCITED', group: 'emotion', accent: '#ff8bd6',
      description: '星形眼睛、张嘴笑和快速弹跳，是完成愿望或收到好消息时的状态。',
      dna: ['眼睛：星形', '眉毛：高挑', '嘴巴：张嘴笑'], sequence: ['眼睛闪烁', '身体快速弹跳', '头顶星星扩散', '呼吸加速'],
      duration: 1250, base: { eyeShape: 'star', mouth: 'open_smile', mouthOpen: .62, brow: 'raised', blush: .2, effects: FX('stars', 'sparkle'), accent: '#ff8bd6' },
      timeline: [K(0, { headY: 3, headScale: 1 }), K(.18, { headY: -12, headScale: 1.06 }), K(.38, { headY: 3, headScale: 1 }), K(.58, { headY: -8, headScale: 1.04 }), K(.8, { headY: 2, headScale: 1 }), K(1, { headY: 3, headScale: 1 })]
    },
    {
      id: 'love', label: '喜欢', name: 'LOVE', group: 'emotion', accent: '#ff82ac',
      description: '心脏形眼睛、深腮红和上浮爱心，用于感谢、喜欢和亲密互动。',
      dna: ['眼睛：心形', '眉毛：柔和', '嘴巴：微笑'], sequence: ['心脏眼睛亮起', '腮红加深', '爱心上浮', '缓慢回正'],
      duration: 2600, base: { eyeShape: 'heart', brow: 'neutral', mouth: 'smile', mouthCurve: .75, blush: .95, effects: FX('hearts'), accent: '#ff82ac' },
      motion: [W('headY', 4, 2300)]
    },
    {
      id: 'shy', label: '害羞', name: 'SHY', group: 'emotion', accent: '#ff82ac',
      description: '视线向下躲开，头部缩回，腮红铺满脸颊并轻轻左右摆动。',
      dna: ['眼睛：向下躲', '眉毛：内压上', '嘴巴：小微笑'], sequence: ['眼神下垂', '头部缩回', '腮红加深', '小幅摆头'],
      duration: 3000, base: { eyeShape: 'sleepy', pupilY: .75, brow: 'worried', mouth: 'small_smile', blush: .85, headY: 8, headScale: .96, accent: '#ff82ac' },
      motion: [W('headRot', 2.5, 2600), W('blush', .05, 2200)]
    },
    {
      id: 'proud', label: '得意', name: 'PROUD', group: 'emotion', accent: '#f6bd4d',
      description: '眼睛变成自信的弧线，嘴角单侧上扬，头部微抬，像是刚刚完成一件漂亮的事。',
      dna: ['眼睛：自信弧', '眉毛：上挑', '嘴巴：单侧笑'], sequence: ['下巴抬起', '眼睛收成弧', '单侧笑', '闪光点出现'],
      duration: 3000, base: { eyeShape: 'crescent', brow: 'raised', mouth: 'smirk', headY: -7, headRot: -2, effects: FX('sparkle'), accent: '#f6bd4d' },
      motion: [W('headY', 2.5, 2800)]
    },
    {
      id: 'satisfied', label: '满足', name: 'SATISFIED', group: 'emotion', accent: '#ffb84d',
      description: '像吃完一顿好饭后的放松表情，眼睛柔和、嘴角平稳上扬、呼吸变慢。',
      dna: ['眼睛：柔和笑眼', '眉毛：放松', '嘴巴：微笑'], sequence: ['眼睛放松', '嘴角上扬', '慢呼吸', '腮红轻显'],
      duration: 4400, base: { eyeShape: 'happy', brow: 'neutral', mouth: 'smile', mouthCurve: .62, blush: .18, accent: '#ffb84d' },
      motion: [W('headY', 3.2, 4400)]
    },
    {
      id: 'relieved', label: '松一口气', name: 'RELIEVED', group: 'emotion', accent: '#4bdb9f',
      description: '先闭眼屏住呼吸，再明显呼气，肩部和头部一起放松。',
      dna: ['眼睛：闭合后睁开', '眉毛：先紧后松', '嘴巴：呼气小嘴'], sequence: ['闭眼憋住', '呼气放松', '睁眼', '嘴角回落'],
      duration: 2600, base: { eyeShape: 'open', brow: 'worried', mouth: 'small', accent: '#4bdb9f' },
      timeline: [K(0, { eyeShape: 'closed', browY: -3, mouthOpen: .12 }), K(.3, { eyeShape: 'closed', browY: 4, mouthOpen: .45, headY: 3 }), K(.55, { eyeShape: 'sleepy', mouth: 'small_smile', headY: 6 }), K(.82, { eyeShape: 'open', mouth: 'smile', headY: 0 }), K(1, { eyeShape: 'open', mouth: 'smile', headY: 0 })]
    },
    {
      id: 'puzzled', label: '疑惑', name: 'PUZZLED', group: 'emotion', accent: '#f6bd4d',
      description: '一侧眉毛挑起、一只眼睛放大，嘴巴变成波浪线，是“等等？”的表情。',
      dna: ['眼睛：左大右小', '眉毛：一侧上挑', '嘴巴：波浪线'], sequence: ['侧头', '单边眉挑', '瞳孔停顿', '问号出现'],
      duration: 2800, base: { leftEyeShape: 'wide', rightEyeShape: 'narrow', brow: 'asym_raised', mouth: 'wavy', headRot: 4, effects: FX('question'), accent: '#f6bd4d' },
      motion: [W('headRot', 2.2, 2700)]
    },
    {
      id: 'confused', label: '困惑', name: 'CONFUSED', group: 'emotion', accent: '#aa9bff',
      description: '双眼朝相反方向扫描，眉毛压紧，嘴巴折成波浪，适合规则冲突和上下文不清楚的时候。',
      dna: ['眼睛：左右扫描', '眉毛：内压', '嘴巴：波浪线'], sequence: ['视线左右扫描', '眉间收紧', '头部小幅晃动', '问号与轨道点'],
      duration: 2100, base: { eyeShape: 'scan', brow: 'focused', mouth: 'wavy', headRot: -3, effects: FX('question', 'orbit'), accent: '#aa9bff' },
      motion: [W('pupilX', .9, 1000), W('headRot', 3, 2100)]
    },
    {
      id: 'surprised', label: '惊讶', name: 'SURPRISED', group: 'emotion', accent: '#65c6ff',
      description: '眼白瞬间放大，眉毛抬高，嘴巴变成小圆，头部向后缩一下。',
      dna: ['眼睛：大圆眼', '眉毛：高高抬起', '嘴巴：圆嘴'], sequence: ['眼睛放大', '眉毛抬起', '头部后缩', '嘴巴圆起'],
      duration: 1500, base: { eyeShape: 'wide', mouth: 'o', brow: 'raised', headY: -5, effects: FX('exclamation'), accent: '#65c6ff' },
      timeline: [K(0, { eyeOpen: .8, headScale: .98 }), K(.2, { eyeOpen: 1.5, headScale: 1.06 }), K(.45, { eyeOpen: 1.2, headScale: 1.02 }), K(.75, { eyeOpen: 1.05, headScale: 1 }), K(1, { eyeOpen: .8, headScale: .98 })]
    },
    {
      id: 'sad', label: '难过', name: 'SAD', group: 'emotion', accent: '#7095c9',
      description: '眼角下垂、眉头向内抬，嘴角向下，整体重心下沉。',
      dna: ['眼睛：下垂', '眉毛：内压上', '嘴巴：撇嘴'], sequence: ['视线下垂', '眉头内抬', '嘴角下落', '头部下沉'],
      duration: 4200, base: { eyeShape: 'sad', brow: 'worried', mouth: 'frown', headY: 8, accent: '#7095c9' },
      motion: [W('headY', 2.8, 4200)]
    },
    {
      id: 'hurt', label: '委屈', name: 'HURT', group: 'emotion', accent: '#ff82ac',
      description: '比难过更亲近，眼睛泛红、泪滴先出现，嘴角向下但仍想看着用户。',
      dna: ['眼睛：湿润下垂', '眉毛：内压上', '嘴巴：小撇嘴'], sequence: ['眼神含水', '泪滴上浮', '嘴角抖动', '头部微缩'],
      duration: 3400, base: { eyeShape: 'sad', brow: 'worried', mouth: 'frown_small', blush: .38, headY: 6, effects: FX('tear'), accent: '#ff82ac' },
      motion: [W('headRot', 1.8, 3200)]
    },
    {
      id: 'angry', label: '生气', name: 'ANGRY', group: 'emotion', accent: '#ff735c',
      description: '眉毛压向内眼角，眼型变窄，嘴巴向下，身体出现短促抖动和红色警示。',
      dna: ['眼睛：窄眼', '眉毛：内压下', '嘴巴：撇嘴'], sequence: ['眉毛下压', '眼睛变窄', '头部短抖', '红色闪光'],
      duration: 1100, base: { eyeShape: 'narrow', brow: 'angry', mouth: 'frown', headRot: 0, effects: FX('anger'), accent: '#ff735c' },
      motion: [W('headX', 3.5, 240), W('headRot', 2.5, 520)]
    },
    {
      id: 'tired', label: '疲惫', name: 'TIRED', group: 'emotion', accent: '#8294aa',
      description: '上眼睑压低、下眼睑有重量，嘴巴无力，呼吸和眨眼间隔都变长。',
      dna: ['眼睛：半闭', '眉毛：下垂', '嘴巴：小嘴'], sequence: ['眼皮下垂', '头部下沉', '慢呼吸', '偶尔闭眼'],
      duration: 5200, base: { eyeShape: 'sleepy', brow: 'sad', mouth: 'small', headY: 6, accent: '#8294aa' },
      motion: [W('headY', 3.5, 5200)]
    },
    {
      id: 'bored', label: '无聊', name: 'BORED', group: 'emotion', accent: '#9aa7b7',
      description: '视线横着飘走，嘴巴平直，头顶出现三个省略号，表示没有新刺激。',
      dna: ['眼睛：向侧看', '眉毛：平眉', '嘴巴：平嘴'], sequence: ['视线漂走', '眼皮放松', '省略号出现', '等待新指令'],
      duration: 4600, base: { eyeShape: 'narrow', pupilX: .9, brow: 'neutral', mouth: 'flat', effects: FX('ellipsis'), accent: '#9aa7b7' },
      motion: [W('pupilX', .35, 4100), W('headRot', 1.6, 4200)]
    },
    {
      id: 'greeting', label: '打招呼', name: 'GREETING', group: 'dialog', accent: '#4bdb9f',
      description: '眼睛向上弯起，嘴巴笑开，头部先点一下再回到正视，像在说“你来了”。',
      dna: ['眼睛：笑眼', '眉毛：上挑', '嘴巴：微笑'], sequence: ['眼神锁定', '点头', '嘴角展开', '闪光收尾'],
      duration: 1800, base: { eyeShape: 'happy', brow: 'raised', mouth: 'smile', blush: .18, effects: FX('sparkle'), accent: '#4bdb9f' },
      timeline: [K(0, { headY: 5 }), K(.22, { headY: -9, headScale: 1.03 }), K(.5, { headY: 2 }), K(1, { headY: 5 })]
    },
    {
      id: 'recognizing', label: '识别', name: 'RECOGNIZING', group: 'dialog', accent: '#65c6ff',
      description: '眼睛聚焦、瞳孔收窄，头部不动，周围有一圈扫描环，适合唤醒后确认声源。',
      dna: ['眼睛：聚焦', '眉毛：平眉', '嘴巴：闭嘴'], sequence: ['聚焦', '扫描环收拢', '确认目标', '进入聆听'],
      duration: 2200, base: { eyeShape: 'scan', brow: 'neutral', mouth: 'small', effects: FX('rings', 'target'), accent: '#65c6ff' },
      motion: [W('eyeW', .08, 1800)]
    },
    {
      id: 'searching', label: '搜索资料', name: 'SEARCHING', group: 'dialog', accent: '#65c6ff',
      description: '眼睛左右扫过文本，头顶轨道点在转，表示正在外部检索或读取资料。',
      dna: ['眼睛：左右扫描', '眉毛：专注', '嘴巴：平嘴'], sequence: ['视线左右扫描', '轨道点旋转', '头部轻微跟随', '等待下一段'],
      duration: 1800, base: { eyeShape: 'scan', brow: 'focused', mouth: 'flat', effects: FX('orbit'), accent: '#65c6ff' },
      motion: [W('pupilX', 1, 850), W('headRot', 2, 1800)]
    },
    {
      id: 'reading', label: '阅读中', name: 'READING', group: 'dialog', accent: '#65c6ff',
      description: '视线集中在下方，眼睛逐行移动，适合朗读答案或查看代码。',
      dna: ['眼睛：向下看', '眉毛：专注', '嘴巴：小嘴'], sequence: ['视线向下', '逐行移动', '头部轻微跟随', '保持安静'],
      duration: 2600, base: { eyeShape: 'narrow', pupilY: .9, brow: 'focused', mouth: 'small', headY: 3, accent: '#65c6ff' },
      motion: [W('pupilX', .45, 1700), W('headY', 1.5, 2600)]
    },
    {
      id: 'coding', label: '处理任务', name: 'CODING', group: 'dialog', accent: '#4bdb9f',
      description: '眉间收紧、眼睛变成短横线，进度环在头顶转动，表示正在执行连续工作。',
      dna: ['眼睛：专注横眼', '眉毛：内压', '嘴巴：平嘴'], sequence: ['进入专注', '进度环旋转', '视线周期性扫描', '完成前保持'],
      duration: 2400, base: { eyeShape: 'scan', brow: 'focused', mouth: 'flat', effects: FX('progress'), accent: '#4bdb9f' },
      motion: [W('pupilX', .8, 1000), W('headRot', 1.4, 2400)]
    },
    {
      id: 'tool_use', label: '调用工具', name: 'TOOL_USE', group: 'dialog', accent: '#aa9bff',
      description: '双眼收成瞄准框，头部保持稳定，画面出现工具节点和连接线。',
      dna: ['眼睛：瞄准', '眉毛：压平', '嘴巴：小嘴'], sequence: ['眼睛锁定', '工具节点出现', '连接线连接', '进入处理中'],
      duration: 2600, base: { eyeShape: 'target', brow: 'focused', mouth: 'small', effects: FX('orbit', 'target'), accent: '#aa9bff' },
      motion: [W('headX', 1.5, 1800)]
    },
    {
      id: 'waiting', label: '等待输入', name: 'WAITING', group: 'dialog', accent: '#f6bd4d',
      description: '眼睛保持适应性的圆睁，嘴型平台，三条省略号来回呼吸，表示轮到你说话了。',
      dna: ['眼睛：自然圆眼', '眉毛：放松', '嘴巴：平嘴'], sequence: ['保持视线', '省略号呼吸', '随时准备接话', '不抢答'],
      duration: 3000, base: { eyeShape: 'open', brow: 'neutral', mouth: 'flat', effects: FX('ellipsis'), accent: '#f6bd4d' },
      motion: [W('headY', 2.4, 3000)]
    },
    {
      id: 'success', label: '完成', name: 'SUCCESS', group: 'dialog', accent: '#4bdb9f',
      description: '眼睛笑起，嘴巴张开，检查符号出现，是任务完成后的高能量反馈。',
      dna: ['眼睛：笑眼', '眉毛：上挑', '嘴巴：微笑'], sequence: ['眼睛先亮', '嘴角上扬', '对勾出现', '闪光扩散'],
      duration: 2200, base: { eyeShape: 'happy', brow: 'raised', mouth: 'smile', blush: .22, effects: FX('check', 'sparkle'), accent: '#4bdb9f' },
      timeline: [K(0, { headY: 3 }), K(.2, { headY: -8, headScale: 1.04 }), K(.5, { headY: 1 }), K(1, { headY: 3 })]
    },
    {
      id: 'error', label: '出错', name: 'ERROR', group: 'dialog', accent: '#ff735c',
      description: '双眼变成交叉、嘴巴向下，头部短促抖动，红叉停在右侧不再回正。',
      dna: ['眼睛：X 形', '眉毛：内压', '嘴巴：撇嘴'], sequence: ['眼睛变叉', '头部短抖', '红色警示', '等待重试'],
      duration: 1300, base: { eyeShape: 'x', brow: 'angry', mouth: 'frown', effects: FX('cross'), accent: '#ff735c' },
      motion: [W('headX', 4, 220), W('headRot', 3, 520)]
    },
    {
      id: 'interrupted', label: '被打断', name: 'INTERRUPTED', group: 'dialog', accent: '#ff735c',
      description: '眼睛突然放大，嘴巴僵住，一道斜线切过声波，表示当前回答被用户中断。',
      dna: ['眼睛：大睁', '眉毛：高抬', '嘴巴：小圆嘴'], sequence: ['眼睛放大', '嘴巴停住', '声音被切断', '等待新指令'],
      duration: 1500, base: { eyeShape: 'wide', brow: 'raised', mouth: 'o', effects: FX('slash', 'exclamation'), accent: '#ff735c' },
      timeline: [K(0, { eyeOpen: .7, headRot: 2 }), K(.2, { eyeOpen: 1.45, headRot: -4 }), K(.5, { eyeOpen: 1.15, headRot: 1 }), K(1, { eyeOpen: .7, headRot: 2 })]
    },
    {
      id: 'music', label: '音乐播放', name: 'MUSIC', group: 'dialog', accent: '#ff8bd6',
      description: '眼睛笑起，嘴型跟着节拍开合，底部音量柱和飘动音符持续反馈播放状态。',
      dna: ['眼睛：笑眼', '眉毛：上挑', '嘴巴：节拍开合'], sequence: ['进入律动', '嘴型跟拍', '音符上浮', '音量柱跳动'],
      duration: 960, base: { eyeShape: 'happy', brow: 'raised', mouth: 'open_smile', blush: .28, effects: FX('note', 'bars'), accent: '#ff8bd6' },
      timeline: [K(0, { mouthOpen: .2, headY: 4 }), K(.25, { mouthOpen: .65, headY: -4 }), K(.5, { mouthOpen: .22, headY: 3 }), K(.75, { mouthOpen: .58, headY: -3 }), K(1, { mouthOpen: .2, headY: 4 })]
    },
    {
      id: 'notification', label: '新通知', name: 'NOTIFICATION', group: 'dialog', accent: '#f6bd4d',
      description: '视线向右上跳，眉毛抬起，一条通知线从右侧穿过，用一次短促动作提示新消息。',
      dna: ['眼睛：向上看', '眉毛：上挑', '嘴巴：小嘴'], sequence: ['视线跳起', '眉毛抬起', '通知线穿过', '回到待机'],
      duration: 1900, base: { eyeShape: 'open', pupilY: -.9, brow: 'raised', mouth: 'small', headRot: 3, effects: FX('notification'), accent: '#f6bd4d' },
      motion: [W('headRot', 2, 1900)]
    },
    {
      id: 'offline', label: '离线', name: 'OFFLINE', group: 'system', accent: '#8294aa',
      description: '脸部和眼睛降低亮度，嘴巴平直，信号斜线表示设备暂时断开连接。',
      dna: ['眼睛：低亮', '眉毛：平眉', '嘴巴：平嘴'], sequence: ['亮度降低', '信号断开', '保持可唤醒', '自动重连'],
      duration: 5000, base: { eyeShape: 'narrow', brow: 'neutral', mouth: 'flat', dim: .55, effects: FX('slash'), accent: '#8294aa' },
      motion: [W('headY', 2, 5000)]
    },
    {
      id: 'reconnecting', label: '重连中', name: 'RECONNECTING', group: 'system', accent: '#65c6ff',
      description: '眼睛跟随旋转环转动，头部保持耐心，连接点和省略号一起循环。',
      dna: ['眼睛：加载眼', '眉毛：专注', '嘴巴：小嘴'], sequence: ['旋转环启动', '眼睛跟随', '连接点闪烁', '成功后转为就绪'],
      duration: 1800, base: { eyeShape: 'loading', brow: 'focused', mouth: 'small', effects: FX('loading', 'ellipsis'), accent: '#65c6ff' },
      motion: [W('headRot', 1.5, 1800)]
    },
    {
      id: 'low_battery', label: '电量低', name: 'LOW_BATTERY', group: 'system', accent: '#ff735c',
      description: '眼睛下垂、眉毛担忧，电量框架变成红色并闪烁，提示尽快充电。',
      dna: ['眼睛：疲惫', '眉毛：担忧', '嘴巴：小嘴'], sequence: ['眼皮下垂', '电量框闪红', '头部下沉', '继续可唤醒'],
      duration: 2800, base: { eyeShape: 'sleepy', brow: 'worried', mouth: 'small', headY: 6, effects: FX('battery_low'), accent: '#ff735c' },
      motion: [W('headY', 2.5, 2800)]
    },
    {
      id: 'charging', label: '充电中', name: 'CHARGING', group: 'system', accent: '#4bdb9f',
      description: '眼睛放松、嘴角上扬，电量图标中出现充能闪电，呼吸逐渐稳定。',
      dna: ['眼睛：放松笑眼', '眉毛：平眉', '嘴巴：微笑'], sequence: ['进入充电', '电量上升', '呼吸变稳', '绿色恢复'],
      duration: 3600, base: { eyeShape: 'happy', brow: 'neutral', mouth: 'smile', blush: .12, effects: FX('charging'), accent: '#4bdb9f' },
      motion: [W('headY', 2.2, 3600)]
    },
    {
      id: 'muted', label: '已静音', name: 'MUTED', group: 'system', accent: '#9aa7b7',
      description: '嘴巴闭合，右侧音量波被切断，眼睛保持平静，不会用错误颜色误导为故障。',
      dna: ['眼睛：平静', '眉毛：放松', '嘴巴：闭嘴'], sequence: ['关闭音量', '声音波被切断', '保持状态', '可再次唤醒'],
      duration: 4200, base: { eyeShape: 'sleepy', brow: 'neutral', mouth: 'flat', effects: FX('mute'), accent: '#9aa7b7' },
      motion: [W('headY', 1.8, 4200)]
    },
    {
      id: 'volume', label: '调整音量', name: 'VOLUME', group: 'system', accent: '#4bdb9f',
      description: '视线向上，嘴巴张开，旁边的音量柱随滑块变化，是对音量键的直接反馈。',
      dna: ['眼睛：向上看', '眉毛：轻抬', '嘴巴：小张嘴'], sequence: ['音量柱变化', '视线跟随', '数值稳定后回正', '记住音量'],
      duration: 1500, base: { eyeShape: 'open', pupilY: -.5, brow: 'raised', mouth: 'o', effects: FX('bars', 'plus'), accent: '#4bdb9f' },
      motion: [W('headY', 2, 1500)]
    },
    {
      id: 'dnd', label: '免打扰', name: 'DND', group: 'system', accent: '#aa9bff',
      description: '闭眼、月亮和星星出现，通知被静默收集，不主动打扰用户。',
      dna: ['眼睛：闭合', '眉毛：放松', '嘴巴：小嘴'], sequence: ['闭眼', '月亮升起', '星星出现', '静默等待'],
      duration: 5200, base: { eyeShape: 'closed', brow: 'neutral', mouth: 'small', effects: FX('moon'), accent: '#aa9bff' },
      motion: [W('headY', 3, 5200)]
    },
    {
      id: 'update', label: '更新中', name: 'UPDATE', group: 'system', accent: '#f6bd4d',
      description: '眼睛跟随进度环，头部保持稳定，明确显示固件或应用正在更新。',
      dna: ['眼睛：加载眼', '眉毛：专注', '嘴巴：小嘴'], sequence: ['进度环启动', '眼睛跟随', '保持稳定', '完成后重启'],
      duration: 2400, base: { eyeShape: 'loading', brow: 'focused', mouth: 'small', effects: FX('progress', 'update'), accent: '#f6bd4d' },
      motion: [W('headY', 1.4, 2400)]
    },
    {
      id: 'locked', label: '已锁定', name: 'LOCKED', group: 'system', accent: '#8294aa',
      description: '眼睛闭合、嘴巴平直，锁形符号出现，表示当前操作需要先解锁。',
      dna: ['眼睛：闭合', '眉毛：平眉', '嘴巴：平嘴'], sequence: ['闭眼', '锁形出现', '停止响应', '等待解锁'],
      duration: 4200, base: { eyeShape: 'closed', brow: 'neutral', mouth: 'flat', effects: FX('lock'), accent: '#8294aa' },
      motion: [W('headY', 1.6, 4200)]
    },
    {
      id: 'warning', label: '注意', name: 'WARNING', group: 'system', accent: '#f6bd4d',
      description: '眉毛抬起、眼睛收窄，嘴型保持小圆，右侧出现黄色感叹号，提醒需要用户确认。',
      dna: ['眼睛：窄眼', '眉毛：高抬', '嘴巴：小圆嘴'], sequence: ['眼睛收窄', '感叹号出现', '头部停住', '等待确认'],
      duration: 1600, base: { eyeShape: 'narrow', brow: 'raised', mouth: 'o', effects: FX('warning'), accent: '#f6bd4d' },
      motion: [W('headY', 2, 1600)]
    }
  ];

  window.DOTII_EXPRESSION_LIBRARY = { groups, expressions };
})();
