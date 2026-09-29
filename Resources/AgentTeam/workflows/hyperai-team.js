export const meta = {
  name: 'hyperai-team',
  description: "Run a request through HyperAI's team: the director plans, the roles build in parallel, the QA tester proves each part, failures go back to their builder, a lighting pass for new levels, and the director reviews",
  whenToUse: 'Any Unreal request with more than one part, such as "a torch-lit crypt with a door that opens when the player finds the key", or when the user asks for the team',
  phases: [
    { title: 'Plan', detail: 'the director splits the request into parts for the roles' },
    { title: 'Build', detail: 'programmer, level designer, lighting artist, technical artist and UI designer; a part starts once the parts it uses are done' },
    { title: 'QA', detail: 'the QA tester proves each part against its checks and its role quality bar' },
    { title: 'Fix', detail: 'failures go back to the builder, at most three rounds' },
    { title: 'Light', detail: 'the lighting artist lights any level the team built and nobody lit' },
    { title: 'Review', detail: 'the director reviews the result against the request and the bar' },
  ],
}

const MAX_FIX_ROUNDS = 3
const ROLES = ['programmer', 'level-designer', 'lighting-artist', 'technical-artist', 'ui-designer']
const request = String(typeof args === 'string' ? args : (args && args.request) || '').trim()
if (!request) {
  log('Say what the team should make, for example: /hyperai-team a torch-lit crypt with a door that opens when the player finds the key')
  return { status: 'nothing_to_do' }
}

const LIST = { type: 'array', items: { type: 'string' } }
const PLAN = {
  type: 'object',
  properties: {
    summary: { type: 'string' },
    parts: {
      type: 'array',
      minItems: 1,
      maxItems: 8,
      items: {
        type: 'object',
        properties: {
          id: { type: 'string' },
          role: { type: 'string', enum: ROLES },
          task: { type: 'string' },
          assets: LIST,
          dependsOn: LIST,
          acceptance: { type: 'array', items: { type: 'string' }, minItems: 1 },
        },
        required: ['id', 'role', 'task', 'assets', 'dependsOn', 'acceptance'],
      },
    },
  },
  required: ['summary', 'parts'],
}
const BUILT = {
  type: 'object',
  properties: { done: { type: 'boolean' }, assets: LIST, notes: { type: 'string' } },
  required: ['done', 'assets', 'notes'],
}
const TESTED = {
  type: 'object',
  properties: {
    passed: { type: 'boolean' },
    checks: {
      type: 'array',
      items: {
        type: 'object',
        properties: { check: { type: 'string' }, passed: { type: 'boolean' }, evidence: { type: 'string' } },
        required: ['check', 'passed', 'evidence'],
      },
    },
    failures: LIST,
  },
  required: ['passed', 'checks', 'failures'],
}

phase('Plan')
const plan = await agent(
  `Plan this request for the Unreal project as parts that the team's roles can build at the same time:\n\n${request}`,
  { agentType: 'director', phase: 'Plan', label: 'plan', schema: PLAN })
if (!plan || !plan.parts.length) {
  log('The director returned no plan; nothing was built.')
  return { status: 'no_plan' }
}
log(`Plan: ${plan.parts.length} part(s). ${plan.summary}`)

// A part starts once the parts it depends on finish. Unknown ids and cycles are dropped so every part can start.
const byId = new Map(plan.parts.map(part => [part.id, part]))
const visiting = new Set()
const visited = new Set()
function trimDependencies(part) {
  if (visited.has(part.id)) return
  visiting.add(part.id)
  const kept = (part.dependsOn || []).filter(id => byId.has(id) && !visiting.has(id) && id !== part.id)
  if (kept.length !== (part.dependsOn || []).length) {
    log(`${part.id}: ignoring dependencies that are unknown or circular`)
  }
  part.dependsOn = kept
  kept.forEach(id => trimDependencies(byId.get(id)))
  visiting.delete(part.id)
  visited.add(part.id)
}
plan.parts.forEach(trimDependencies)

const results = new Map()
const describe = part => [
  `Part ${part.id} (${part.role}): ${part.task}`,
  `Assets you own; touch nothing else: ${part.assets.join(', ') || 'none named'}`,
  `Acceptance checks:\n- ${part.acceptance.join('\n- ')}`,
].join('\n\n')
const upstream = part => part.dependsOn
  .map(id => results.get(id))
  .filter(Boolean)
  .map(r => `${r.part.id} (${r.status}): ${r.built ? `${r.built.assets.join(', ')}. ${r.built.notes}` : 'not built'}`)
  .join('\n')

// Build, then QA against the checks and the role's bar, then fix, until it passes or the rounds run out.
async function runPart(part, context) {
  const deps = context || upstream(part)
  let built = await agent(
    `Build this part of "${request}" to your quality bar.\n\n${describe(part)}${deps ? `\n\nWhat it builds on:\n${deps}` : ''}`,
    { agentType: part.role, phase: part.id === 'lighting-pass' ? 'Light' : 'Build', label: `build:${part.id}`, schema: BUILT })
  if (!built) return { part, status: 'not_built', fixRounds: 0 }
  for (let round = 0; ; round++) {
    const tested = await agent(
      `Test this part of "${request}" against its acceptance checks and the ${part.role}'s quality bar in .hyperai/agents/${part.role}.md.\n\n` +
      `${describe(part)}\n\nThe builder reports: ${JSON.stringify(built)}`,
      { agentType: 'qa-tester', phase: 'QA', label: `qa:${part.id}${round ? `:${round}` : ''}`, schema: TESTED })
    if (!tested) return { part, built, status: 'untested', fixRounds: round }
    if (tested.passed) return { part, built, tested, status: 'passed', fixRounds: round }
    if (round >= MAX_FIX_ROUNDS) return { part, built, tested, status: 'failed', fixRounds: round }
    log(`${part.id}: ${tested.failures.length} failure(s); fix round ${round + 1}`)
    const fixed = await agent(
      `Fix this part of "${request}". The QA tester found:\n- ${tested.failures.join('\n- ')}\n\n${describe(part)}\n\nYour last result: ${JSON.stringify(built)}`,
      { agentType: part.role, phase: 'Fix', label: `fix:${part.id}:${round + 1}`, schema: BUILT })
    if (fixed) built = fixed
  }
}

const running = new Map()
function start(part) {
  if (!running.has(part.id)) {
    running.set(part.id, Promise.all(part.dependsOn.map(id => start(byId.get(id))))
      .then(() => runPart(part))
      .catch(error => ({ part, status: 'not_built', fixRounds: 0, error: String(error) }))
      .then(result => {
        results.set(part.id, result)
        log(`${part.id}: ${result.status}`)
        return result
      }))
  }
  return running.get(part.id)
}
const finished = await Promise.all(plan.parts.map(start))

// A level the team built or changed gets lit, unless a part already lit it.
const levelParts = finished.filter(r => r.part.role === 'level-designer' && r.built)
if (levelParts.length && !plan.parts.some(part => part.role === 'lighting-artist')) {
  phase('Light')
  const lighting = {
    id: 'lighting-pass',
    role: 'lighting-artist',
    task: `Light what the team built for "${request}" to your quality bar: the mood the request asks for, the goal of each space lit or framed, ` +
      'exposure locked, and captures from where the player stands.',
    assets: [],
    dependsOn: [],
    acceptance: [
      'On a locked-exposure capture from the player view, luminanceP95 is under 0.95 and luminanceP5 over 0.02',
      'There is a clear key light direction, and the colour temperature fits the time of day',
      'The goal of each space is lit or framed so the way forward reads',
    ],
  }
  const built = levelParts.map(r => `${r.part.id}: ${r.built.assets.join(', ')}. ${r.built.notes}`).join('\n')
  const result = await runPart(lighting, built)
  finished.push(result)
  log(`lighting-pass: ${result.status}`)
}

phase('Review')
const summary = finished.map(r => ({
  id: r.part.id,
  role: r.part.role,
  status: r.status,
  fixRounds: r.fixRounds,
  built: r.built || null,
  tested: r.tested || null,
}))
const report = await agent(
  `Review what the team built for: "${request}". Judge it against each role's quality bar, not only the checks.\n\nPlan: ${plan.summary}\n\n` +
  `Parts:\n${JSON.stringify(summary, null, 1)}`,
  { agentType: 'director', phase: 'Review', label: 'review' })
return {
  request,
  plan: plan.summary,
  parts: summary.map(s => ({ id: s.id, role: s.role, status: s.status, assets: s.built ? s.built.assets : [] })),
  report,
}
