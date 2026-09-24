export const meta = {
  name: 'hyperai-verify',
  description: 'Verify gameplay in Unreal within a minute: playtesters write test cases in parallel, HyperAI plays every case at once in copies of the level, and playtesters judge the evidence',
  whenToUse: 'After gameplay is built, to check that it works, such as "the coin adds 10 to the score and disappears, and the door opens when the player walks up to it"',
  phases: [
    { title: 'Plan', detail: 'a playtester reads the case limit and splits the request into checks' },
    { title: 'Write', detail: 'one playtester per check writes its test case' },
    { title: 'Run', detail: 'one hyper_verify_run plays every case at once, within its deadline' },
    { title: 'Judge', detail: 'one playtester per case decides whether its evidence proves the check' },
  ],
}

const request = String(typeof args === 'string' ? args : (args && args.request) || '').trim()
if (!request) {
  log('Say what to verify, for example: /hyperai-verify the coin adds 10 to the score and disappears')
  return { status: 'nothing_to_verify' }
}

const ACTIONS = ['MoveTo', 'LookAt', 'Jump', 'Press', 'Hold', 'Interact', 'Wait', 'WaitUntil', 'Survey', 'Sample',
  'AssertProperty', 'AssertNear', 'AssertExists', 'AssertGone']
const STEP = {
  type: 'object',
  properties: {
    action: { type: 'string', enum: ACTIONS },
    until: { type: 'string', enum: ['AssertProperty', 'AssertNear', 'AssertExists', 'AssertGone'] },
    target: { type: 'string' },
    other: { type: 'string' },
    location: {
      type: 'object',
      properties: { x: { type: 'number' }, y: { type: 'number' }, z: { type: 'number' } },
      required: ['x', 'y', 'z'],
    },
    property: { type: 'string' },
    compare: { type: 'string', enum: ['==', '!=', '<', '<=', '>', '>=', 'contains'] },
    value: { type: 'string' },
    inputAction: { type: 'string' },
    seconds: { type: 'number' },
    radius: { type: 'number' },
    timeoutSeconds: { type: 'number' },
  },
  required: ['action'],
}
const PLAN = {
  type: 'object',
  properties: {
    limit: { type: 'integer', minimum: 1, maximum: 10 },
    level: { type: 'string' },
    notes: { type: 'string' },
    checks: {
      type: 'array',
      minItems: 1,
      maxItems: 10,
      items: {
        type: 'object',
        properties: { name: { type: 'string' }, requirement: { type: 'string' } },
        required: ['name', 'requirement'],
      },
    },
  },
  required: ['limit', 'level', 'notes', 'checks'],
}
const CASE = {
  type: 'object',
  properties: {
    name: { type: 'string' },
    steps: { type: 'array', minItems: 1, maxItems: 32, items: STEP },
    timeoutSeconds: { type: 'number', minimum: 1, maximum: 55 },
    proves: { type: 'string' },
  },
  required: ['name', 'steps', 'timeoutSeconds', 'proves'],
}
const REPORT = {
  type: 'object',
  properties: {
    status: { type: 'string' },
    diagnostic: { type: 'string' },
    wallSeconds: { type: 'number' },
    cases: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          name: { type: 'string' },
          status: { type: 'string' },
          diagnostic: { type: 'string' },
          steps: { type: 'array', items: { type: 'object' } },
        },
        required: ['name', 'status', 'diagnostic', 'steps'],
      },
    },
  },
  required: ['status', 'diagnostic', 'wallSeconds', 'cases'],
}
const VERDICT = {
  type: 'object',
  properties: {
    verdict: { type: 'string', enum: ['pass', 'fail', 'inconclusive'] },
    reason: { type: 'string' },
    evidence: { type: 'string' },
  },
  required: ['verdict', 'reason', 'evidence'],
}

phase('Plan')
const plan = await agent(
  `Plan how to verify this gameplay in the open Unreal level, following the unreal-playtest skill's "Verify many things at once" section:\n\n${request}\n\n` +
  'First call hyper_verify_calibrate with {"bMeasure": false} and report its limit. Then run one hyper_verify_run case of a single Survey step (or one per actor you need), to learn the class names, labels and where the player starts. ' +
  'Split the request into at most that many checks, each one observable thing with one clear pass condition. Write down in notes the targets, properties and input actions the case writers need.',
  { agentType: 'playtester', phase: 'Plan', label: 'plan', schema: PLAN })
if (!plan || !plan.checks.length) {
  log('The planner returned no checks; nothing was verified.')
  return { status: 'no_plan' }
}
const checks = plan.checks.slice(0, Math.max(1, plan.limit))
if (checks.length < plan.checks.length) {
  log(`Only the first ${checks.length} of ${plan.checks.length} checks fit one batch on this machine; the rest are not verified.`)
}
log(`${checks.length} check(s) on ${plan.level}.`)

phase('Write')
const written = await parallel(checks.map(check => () => agent(
  `Write one hyper_verify_run test case for this check, following the unreal-playtest skill's "Verify many things at once" section. Do not run it.\n\n` +
  `Check "${check.name}": ${check.requirement}\n\nWhat the planner learned about the level:\n${plan.notes}\n\n` +
  'The case starts where the game starts the player. Use only the listed step actions, end with an Assert step that proves the requirement, ' +
  'and set timeoutSeconds to the simulated seconds the case needs plus a margin (at most 55). In proves, say what a pass shows.',
  { agentType: 'playtester', phase: 'Write', label: `write:${check.name}`, schema: CASE })))
// Each case keeps its check's name, so results and verdicts line up with the checks.
const cases = written.map((c, index) => c && { ...c, name: checks[index].name }).filter(Boolean)
if (!cases.length) {
  log('No case was written; nothing was verified.')
  return { status: 'no_cases', level: plan.level }
}

phase('Run')
const runArguments = {
  request: {
    cases: cases.map(c => ({ name: c.name, steps: c.steps, timeoutSeconds: c.timeoutSeconds })),
    deadlineSeconds: 60,
  },
}
const report = await agent(
  'Call hyper_verify_run (toolset HyperAIStudio.HyperAIStudioPIEPlaytestToolset) exactly once, with exactly these arguments, changing nothing:\n\n' +
  `${JSON.stringify(runArguments)}\n\n` +
  'Return its returnValue as your structured result: status, diagnostic, wallSeconds and every case with its steps. If the call is refused, return that status and diagnostic with no cases.',
  { agentType: 'playtester', phase: 'Run', label: 'run', schema: REPORT })
if (!report) {
  log('The run step returned nothing.')
  return { status: 'not_run', level: plan.level, cases: cases.map(c => c.name) }
}
log(`hyper_verify_run: ${report.status} in ${report.wallSeconds.toFixed(1)} s. ${report.diagnostic}`)

phase('Judge')
const judged = await parallel(report.cases.map(result => () => {
  const written_ = cases.find(c => c.name === result.name)
  const check = checks.find(c => c.name === result.name)
  return agent(
    `Judge one verification case. Decide only from its evidence whether it proves the requirement.\n\n` +
    `Requirement "${result.name}": ${check ? check.requirement : '(unknown)'}\n` +
    `The case was meant to prove: ${written_ ? written_.proves : '(unknown)'}\n` +
    `Its steps: ${JSON.stringify(written_ ? written_.steps : [])}\n\n` +
    `Its result: ${JSON.stringify(result)}\n\n` +
    'pass: the evidence shows the requirement holds. fail: the evidence shows it does not, which is a bug in the game. ' +
    'inconclusive: the case could not tell, for example a wrong target name or a step that failed for a reason unrelated to the requirement. Quote the evidence line you relied on.',
    { agentType: 'playtester', phase: 'Judge', label: `judge:${result.name}`, schema: VERDICT })
}))

const results = report.cases.map((result, index) => ({
  check: result.name,
  requirement: (checks.find(c => c.name === result.name) || {}).requirement || '',
  caseStatus: result.status,
  verdict: judged[index] ? judged[index].verdict : 'inconclusive',
  reason: judged[index] ? judged[index].reason : 'The judge returned nothing.',
  evidence: judged[index] ? judged[index].evidence : result.diagnostic,
}))
const passed = results.filter(r => r.verdict === 'pass').length
log(`${passed} of ${results.length} check(s) verified as passing.`)
return {
  request,
  level: plan.level,
  runStatus: report.status,
  runSeconds: report.wallSeconds,
  passed,
  total: results.length,
  results,
}
