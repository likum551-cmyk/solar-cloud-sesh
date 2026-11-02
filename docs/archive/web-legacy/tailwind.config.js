/** @type {import('tailwindcss').Config} */
// Colour lives in styles.css tokens (the pause-menu palette); Tailwind
// supplies layout utilities and the one type family only.
export default {
  content: ['./index.html', './src/**/*.{js,jsx}'],
  theme: {
    extend: {
      fontFamily: {
        sans: ['Overpass', 'Bahnschrift', 'Segoe UI', 'sans-serif'],
      },
    },
  },
  plugins: [],
};
