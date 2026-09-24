export const meta = {
  name: 'hyperai-match',
  description: 'Make a scene, material, effect or UI look like a reference image: a builder iterates capture, compare and one change at a time until the score reaches the target or stops improving, then a reviewer judges the best attempt by eye',
  whenToUse: 'When the user has a reference image (concept art, a photo, a generated image) and wants the level, a material, an effect or a UI to look like it, such as "/hyperai-match Content/References/dusk.png make the level lighting match"',
  phases: [
    { title: 'Plan', detail: 'look at the reference, choose the mode, target, capture and levers' },
    { title: 'Match', detail: 'capture, hyper_image_compare in one session, one change at a time' },
    { title: 'Judge', detail: 'a reviewer compares the reference with the best attempt' },
  ],
}

const request = String(typeof args === 'string' ? args : (args && args.request) || '').trim()
if (!request) {
  log('Say what to match and against which image, for example: /hyperai-match Content/References/dusk.png make the level lighting match')
  return { status: 'nothing_to_match' }
}

const PLAN = {
  type: 'object',
  properties: {
    reference: { type: 'string' },
    domain: { type: 'string', enum: ['scene', 'environment', 'material', 'effect', 'ui', 'other'] },
    mode: { type: 'string', enum: ['look', 'aligned'] },
    targetScore: { type: 'number', minimum: 40, maximum: 100 },
    sessionId: { type: 'string' },
    look: { type: 'string' },
    capture: { type: 'string' },
    levers: { type: 'array', minItems: 1, items: { type: 'string' } },
    firstScore: { type: 'number' },
  },
  required: ['reference', 'domain', 'mode', 'targetScore', 'sessionId', 'look', 'capture', 'levers', 'firstScore'],
}
const MATCH = {
  type: 'object',
  properties: {
    sessionId: { type: 'string' },
    stoppedBecause: { type: 'string', enum: ['target', 'plateau', 'limit', 'blocked'] },
    bestIteration: { type: 'integer' },
    bestScore: { type: 'number' },
    bestSideBySide: { type: 'string' },
    iterations: {
      type: 'array',
      items: {
        type: 'object',
        properties: { index: { type: 'integer' }, change: { type: 'string' }, score: { type: 'number' } },
        required: ['index', 'change', 'score'],
      },
    },
    remaining: { type: 'string' },
  },
  required: ['sessionId', 'stoppedBecause', 'bestIteration', 'bestScore', 'bestSideBySide', 'iterations', 'remaining'],
}
const VERDICT = {
  type: 'object',
  properties: {
    verdict: { type: 'string', enum: ['matched', 'close', 'not_matched'] },
    reason: { type: 'string' },
    nextSteps: { type: 'array', items: { type: 'string' } },
  },
  required: ['verdict', 'reason', 'nextSteps'],
}

phase('Plan')
const plan = await agent(
  `Plan an image match in the open Unreal project, following the image-match skill:\n\n${request}\n\n` +
  'Find the reference image the request names (a project-relative PNG or JPG) and open it with your image reader. ' +
  'In look, say in one sentence what makes its look. Choose the domain, the mode (aligned only if your capture can frame the same view), ' +
  'the target score, a new sessionId of letters, digits, - and _, how you will capture each attempt (the exact hyper_capture_view request, ' +
  'or the steps to set up a camera first), and the levers you will change, strongest first. ' +
  'Then capture the current state once and call hyper_image_compare with that sessionId and the note "starting point"; report its score as firstScore. ' +
  'Lock exposure first for a scene or environment match.',
  { agentType: 'level-builder', phase: 'Plan', label: 'plan', schema: PLAN })
if (!plan) {
  log('The planner returned nothing; nothing was matched.')
  return { status: 'no_plan' }
}
log(`Matching ${plan.domain} against ${plan.reference} in ${plan.mode} mode, target ${plan.targetScore}; starting at ${Math.round(plan.firstScore)}.`)

phase('Match')
const builder = plan.domain === 'scene' || plan.domain === 'environment' ? 'level-builder' : 'asset-builder'
const match = await agent(
  `Run the image-match loop from the image-match skill until it stops.\n\n` +
  `Reference: ${plan.reference}\nWhat makes its look: ${plan.look}\nDomain: ${plan.domain}\nMode: ${plan.mode}, target ${plan.targetScore}\n` +
  `Session: ${plan.sessionId} (iteration 0, the starting point, scored ${plan.firstScore})\n` +
  `Capture each attempt: ${plan.capture}\nLevers, strongest first: ${plan.levers.join('; ')}\n\n` +
  'Each iteration: make one change (or one coherent group), capture, and call hyper_image_compare with the same sessionId, the same reference, ' +
  'mode and targetScore, and a note saying what changed. Open its sideBySidePath and look before choosing the next change. ' +
  'Stop at bMatched, at a second bPlateau (after the first, switch lever categories), or after 12 iterations. ' +
  'Do not save the level or assets. Return every iteration with its change and score, the best iteration and its side-by-side path, and what still differs.',
  { agentType: builder, phase: 'Match', label: `match:${plan.domain}`, schema: MATCH })
if (!match) {
  log('The match loop returned nothing.')
  return { status: 'not_matched', plan }
}
log(`Stopped (${match.stoppedBecause}) after ${match.iterations.length} iteration(s); best ${Math.round(match.bestScore)} at iteration ${match.bestIteration}.`)

phase('Judge')
const verdict = await agent(
  `Judge an image match by eye. Open the reference ${plan.reference} and the best attempt's side-by-side ${match.bestSideBySide} ` +
  `(reference left, attempt right, worst grid cells outlined).\n\nThe look to reach: ${plan.look}\n` +
  `Scores by iteration: ${JSON.stringify(match.iterations)}\nWhat the builder says still differs: ${match.remaining}\n\n` +
  'matched: a viewer would call it the same look. close: the look reads, with differences you can name. not_matched: it does not read as the reference. ' +
  'Say why in terms of light, palette, shapes and mood, and list the next steps that would close the gap.',
  { agentType: 'reviewer', phase: 'Judge', label: 'judge', schema: VERDICT })

return {
  request,
  reference: plan.reference,
  domain: plan.domain,
  mode: plan.mode,
  targetScore: plan.targetScore,
  sessionId: match.sessionId,
  startScore: plan.firstScore,
  bestScore: match.bestScore,
  bestIteration: match.bestIteration,
  stoppedBecause: match.stoppedBecause,
  iterations: match.iterations,
  verdict: verdict ? verdict.verdict : 'unjudged',
  reason: verdict ? verdict.reason : 'The reviewer returned nothing.',
  nextSteps: verdict ? verdict.nextSteps : [],
  preview: 'Open the HyperAI Preview tab, pick the Image match source and the same reference, to see every iteration.',
}
