import { useEffect } from 'react';
import Lenis from 'lenis';

/**
 * Smooth, damped wheel scrolling for one scroll container. Lenis drives the
 * container's native scrollTop, so sticky headers, keyboard scrolling and
 * scrollIntoView keep working. Skipped entirely under reduced motion.
 */
export function useLenis(wrapperRef, contentRef, deps = []) {
  useEffect(() => {
    const wrapper = wrapperRef.current;
    const content = contentRef.current;
    if (!wrapper || !content) return undefined;
    if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) return undefined;
    const lenis = new Lenis({
      wrapper,
      content,
      lerp: 0.14,
      wheelMultiplier: 0.9,
      smoothWheel: true,
      autoRaf: true,
    });
    return () => lenis.destroy();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, deps);

  // "More below" cue, with or without smoothing: the wrapper carries
  // data-more while content continues past its bottom edge, and styles.css
  // fades that edge so a cut-off list never looks finished.
  useEffect(() => {
    const wrapper = wrapperRef.current;
    const content = contentRef.current;
    if (!wrapper || !content) return undefined;
    const update = () => {
      wrapper.toggleAttribute('data-more', wrapper.scrollHeight - wrapper.clientHeight - wrapper.scrollTop > 4);
    };
    update();
    wrapper.addEventListener('scroll', update, { passive: true });
    const ro = new ResizeObserver(update);
    ro.observe(wrapper);
    ro.observe(content);
    return () => {
      wrapper.removeEventListener('scroll', update);
      ro.disconnect();
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, deps);
}
