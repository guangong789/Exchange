import { useState } from 'react';
import { evaluateAnswer, type QuizQuestion } from './lesson';

export function QuizCard({
  question,
  number,
  passed,
  onPass,
}: {
  question: QuizQuestion;
  number: number;
  passed: boolean;
  onPass: () => void;
}) {
  const [answer, setAnswer] = useState<string | null>(null);
  const wrong = answer !== null && !evaluateAnswer(question, answer);
  function choose(id: string) {
    setAnswer(id);
    if (evaluateAnswer(question, id)) onPass();
  }
  return (
    <section
      className={`quiz-card ${passed ? 'quiz-passed' : ''}`}
      aria-labelledby={`quiz-${question.id}`}
    >
      <div className="quiz-card-heading">
        <span className="quiz-number mono">{passed ? '✓' : `0${number}`}</span>
        <h3 id={`quiz-${question.id}`}>{question.prompt}</h3>
        {passed && <span className="passed-label">已掌握</span>}
      </div>
      {passed ? (
        <p className="quiz-success">
          {answer && !wrong
            ? question.explanation
            : question.id === 'high'
              ? '你已完成最高价识别。可以继续探索其他 K 线。'
              : '你已认识上影线与实体顶部的关系。'}
        </p>
      ) : (
        <div className={`quiz-options ${question.id === 'high' ? 'numeric-options' : ''}`}>
          {question.options.map((option, i) => (
            <button
              type="button"
              key={option.id}
              className={answer === option.id && wrong ? 'wrong-option' : ''}
              disabled={wrong}
              onClick={() => choose(option.id)}
            >
              <span className="option-letter mono">{String.fromCharCode(65 + i)}</span>
              {option.label}
            </button>
          ))}
        </div>
      )}
      <div className="quiz-feedback" aria-live="polite">
        {wrong && !passed && (
          <>
            <span>
              再看一眼{question.id === 'high' ? '最高的端点' : '实体上方的细线'}，试试看。
            </span>
            <button type="button" className="text-button" onClick={() => setAnswer(null)}>
              重新选择 ↻
            </button>
          </>
        )}
        {passed && <span className="sr-only">回答正确。</span>}
      </div>
    </section>
  );
}
