export const meta = {
  name: 'hyperai-build-system',
  description: 'Build a gameplay system in Unreal with the HyperAI agent team: plan, build the parts in parallel, playtest, fix, review',
  whenToUse: 'A gameplay feature with several parts (Blueprints, materials, VFX or UI, level setup), such as "a coin pickup that adds to a score shown on the HUD"',
  phases: [
    { title: 'Plan', detail: 'gameplay-architect splits the request into parts' },
    { title: 'Build', detail: 'one builder per part; a part starts once the parts it uses are done' },
    { title: 'Playtest', detail: 'playtester checks each part against its acceptance checks' },
    { title: 'Fix', detail: 'failures go back to the builder, at most two rounds' },
    { title: 'Review', detail: 'reviewer reports what was built and verified' },
  ],
}

const MAX_FIX_ROUNDS = 2
const BUILDERS = ['blueprint-builder', 'asset-builder', 'level-builder']
const request = String(typeof args === 'string' ? args : (args && args.request) || '').trim()
if (!request) {
  log('Say what to build, for example: /hyperai-build-system a coin pickup that adds to a score shown on the HUD')
  return { status: 'nothing_to_build' }
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
          builder: { type: 'string', enum: BUILDERS },
          task: { type: 'string' },
          assets: LIST,
          dependsOn: LIST,
          acceptance: { type: 'array', items: { type: 'string' }, minItems: 1 },
        },
        required: ['id', 'builder', 'task', 'assets', 'dependsOn', 'acceptance'],
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
  `Plan this gameplay system for the Unreal project as parts that separate builders can make at the same time:\n\n${request}`,
  { agentType: 'gameplay-architect', phase: 'Plan', label: 'plan', schema: PLAN })
if (!plan || !plan.parts.length) {
  log('The architect returned no plan; nothing was built.')
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
  `Part ${part.id}: ${part.task}`,
  `Assets you own; touch nothing else: ${part.assets.join(', ') || 'none named'}`,
  `Acceptance checks:\n- ${part.acceptance.join('\n- ')}`,
].join('\n\n')
const upstream = part => part.dependsOn
  .map(id => results.get(id))
  .filter(Boolean)
  .map(r => `${r.part.id} (${r.status}): ${r.built ? `${r.built.assets.join(', ')}. ${r.built.notes}` : 'not built'}`)
  .join('\n')

async function runPart(part) {
  const deps = upstream(part)
  let built = await agent(
    `Build this part of "${request}".\n\n${describe(part)}${deps ? `\n\nParts it builds on:\n${deps}` : ''}`,
    { agentType: part.builder, phase: 'Build', label: `build:${part.id}`, schema: BUILT })
  if (!built) return { part, status: 'not_built', fixRounds: 0 }
  for (let round = 0; ; round++) {
    const tested = await agent(
      `Check this part of "${request}" against its acceptance checks.\n\n${describe(part)}\n\nThe builder reports: ${JSON.stringify(built)}`,
      { agentType: 'playtester', phase: 'Playtest', label: `test:${part.id}${round ? `:${round}` : ''}`, schema: TESTED })
    if (!tested) return { part, built, status: 'untested', fixRounds: round }
    if (tested.passed) return { part, built, tested, status: 'passed', fixRounds: round }
    if (round >= MAX_FIX_ROUNDS) return { part, built, tested, status: 'failed', fixRounds: round }
    log(`${part.id}: ${tested.failures.length} failure(s); fix round ${round + 1}`)
    const fixed = await agent(
      `Fix this part of "${request}". The playtester found:\n- ${tested.failures.join('\n- ')}\n\n${describe(part)}\n\nYour last result: ${JSON.stringify(built)}`,
      { agentType: part.builder, phase: 'Fix', label: `fix:${part.id}:${round + 1}`, schema: BUILT })
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

phase('Review')
const summary = finished.map(r => ({
  id: r.part.id,
  builder: r.part.builder,
  status: r.status,
  fixRounds: r.fixRounds,
  built: r.built || null,
  tested: r.tested || null,
}))
const report = await agent(
  `Review what the team built for: "${request}".\n\nPlan: ${plan.summary}\n\nParts:\n${JSON.stringify(summary, null, 1)}`,
  { agentType: 'reviewer', phase: 'Review', label: 'review' })
return {
  request,
  plan: plan.summary,
  parts: summary.map(s => ({ id: s.id, status: s.status, assets: s.built ? s.built.assets : [] })),
  report,
}
